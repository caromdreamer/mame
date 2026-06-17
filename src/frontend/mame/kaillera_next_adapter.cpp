// license:BSD-3-Clause

#include "emu.h"
#include "kaillera_next_adapter.h"

#include "../../../../../sdk/kaillera_next.h"

#include "osdepend.h"
#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using kn_client_create_fn = KnResult (*)(const KnConfig *, const KnCallbacks *, KnClient **);
using kn_client_destroy_fn = void (*)(KnClient *);
using kn_client_tick_fn = KnResult (*)(KnClient *);
using kn_client_poll_network_fn = KnResult (*)(KnClient *);
using kn_client_get_metrics_fn = KnResult (*)(KnClient *, KnMetrics *);
using kn_client_leave_fn = void (*)(KnClient *);

constexpr u8 KN_INPUT_UP = 0x01;
constexpr u8 KN_INPUT_LEFT = 0x02;
constexpr u8 KN_INPUT_RIGHT = 0x04;
constexpr u8 KN_INPUT_DOWN = 0x08;
constexpr u8 KN_INPUT_COIN = 0x10;
constexpr u8 KN_INPUT_START = 0x20;
constexpr u8 KN_INPUT_BUTTON1 = 0x40;
constexpr u8 KN_INPUT_BUTTON2 = 0x80;

const char *env_value(const char *name, const char *fallback)
{
	const char *value = std::getenv(name);
	return (value && value[0]) ? value : fallback;
}

u32 env_u32(const char *name, u32 fallback)
{
	const char *value = std::getenv(name);
	if (!value || !value[0])
		return fallback;

	char *end = nullptr;
	unsigned long parsed = std::strtoul(value, &end, 10);
	if ((end && *end) || parsed > 0xffffffffUL)
		return fallback;

	return u32(parsed);
}

bool env_enabled(const char *name)
{
	const char *value = std::getenv(name);
	return value && value[0] && std::strcmp(value, "0");
}

u8 scripted_pacman_input(u32 frame, u32 player)
{
	u8 input = 0;
	if (env_enabled("KN_MAME_SCRIPT_START"))
	{
		if (player == 0)
		{
			if (frame < 30)
				input |= KN_INPUT_COIN;
			if (frame >= 45 && frame < 75)
				input |= KN_INPUT_START;
		}
		else if (player == 1 && frame >= 75 && frame < 105)
		{
			input |= KN_INPUT_START;
		}
	}

	const u32 phase = (frame / 24 + player) % 4;
	switch (phase)
	{
	case 0:
		return input | KN_INPUT_RIGHT | KN_INPUT_BUTTON1;
	case 1:
		return input | KN_INPUT_DOWN | KN_INPUT_BUTTON2;
	case 2:
		return input | KN_INPUT_LEFT | KN_INPUT_BUTTON1;
	default:
		return input | KN_INPUT_UP;
	}
}

u8 scripted_start_input(u32 frame, u32 player)
{
	if (!env_enabled("KN_MAME_AUTO_START") || player != 0)
		return 0;

	u8 input = 0;
	if (frame >= 30 && frame < 45)
		input |= KN_INPUT_COIN;
	if (frame >= 75 && frame < 90)
		input |= KN_INPUT_START;
	return input;
}

u64 fnv1a64(const u8 *bytes, std::size_t len)
{
	u64 hash = 1469598103934665603ULL;
	for (std::size_t i = 0; i < len; i++)
	{
		hash ^= bytes[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

template <typename T>
bool load_symbol(void *library, const char *name, T &out)
{
	out = reinterpret_cast<T>(dlsym(library, name));
	if (!out)
	{
		osd_printf_error("Kaillera Next: missing SDK symbol %s\n", name);
		return false;
	}
	return true;
}

struct mapped_input_field
{
	ioport_field *field;
	u32 player;
	u8 bit;
};

} // namespace

struct kaillera_next_adapter::impl
{
	explicit impl(running_machine &machine) :
		machine(machine)
	{
	}

	running_machine &machine;
	void *library = nullptr;
	KnClient *client = nullptr;
	kn_client_create_fn kn_client_create = nullptr;
	kn_client_destroy_fn kn_client_destroy = nullptr;
	kn_client_tick_fn kn_client_tick = nullptr;
	kn_client_poll_network_fn kn_client_poll_network = nullptr;
	kn_client_get_metrics_fn kn_client_get_metrics = nullptr;
	kn_client_leave_fn kn_client_leave = nullptr;
	std::unordered_map<u32, std::vector<u8>> snapshots;
	u32 player_id = 0;
	u32 player_count = 1;
	u32 input_size = 1;
	u32 frame_count = 0;
	u32 save_count = 0;
	u32 load_count = 0;
	u32 advance_count = 0;
	u32 injected_frame_count = 0;
	u32 nonneutral_input_count = 0;
	u32 last_snapshot_size = 0;
	u32 last_paced_frame = 0;
	u8 last_local_input = 0;
	u8 last_applied_input[2] = {0xff, 0xff};
	std::chrono::steady_clock::time_point next_frame_at = {};
	std::vector<mapped_input_field> input_map;
	bool warned_missing_ports = false;
	bool input_map_built = false;
	bool trace = false;
	bool initialized = false;
	bool reported_exit = false;
	bool in_advance = false;
	bool networked = false;
};

static void set_field(ioport_field *field, bool pressed, bool lockout = true)
{
	if (!field)
		return;
	field->live().lockout = lockout;
	field->set_value(pressed ? 1 : 0);
}

static bool field_pressed(running_machine &machine, ioport_field *field)
{
	return field && machine.input().seq_pressed(field->seq());
}

static u8 bit_for_field(ioport_field &field)
{
	switch (field.type())
	{
	case IPT_JOYSTICK_UP:
	case IPT_JOYSTICKLEFT_UP:
	case IPT_JOYSTICKRIGHT_UP:
		return KN_INPUT_UP;
	case IPT_JOYSTICK_LEFT:
	case IPT_JOYSTICKLEFT_LEFT:
	case IPT_JOYSTICKRIGHT_LEFT:
		return KN_INPUT_LEFT;
	case IPT_JOYSTICK_RIGHT:
	case IPT_JOYSTICKLEFT_RIGHT:
	case IPT_JOYSTICKRIGHT_RIGHT:
		return KN_INPUT_RIGHT;
	case IPT_JOYSTICK_DOWN:
	case IPT_JOYSTICKLEFT_DOWN:
	case IPT_JOYSTICKRIGHT_DOWN:
		return KN_INPUT_DOWN;
	case IPT_START:
	case IPT_SELECT:
		return field.type() == IPT_START ? KN_INPUT_START : KN_INPUT_COIN;
	default:
		break;
	}

	if (field.type() >= IPT_COIN1 && field.type() <= IPT_COIN12)
		return KN_INPUT_COIN;
	if (field.type() >= IPT_START1 && field.type() <= IPT_START10)
		return KN_INPUT_START;
	if (field.type() >= IPT_BUTTON1 && field.type() <= IPT_BUTTON16)
	{
		u32 const button = u32(field.type()) - u32(IPT_BUTTON1);
		if (button == 0)
			return KN_INPUT_BUTTON1;
		if (button == 1)
			return KN_INPUT_BUTTON2;
	}
	return 0;
}

static u32 player_for_field(ioport_field &field)
{
	if (field.type() >= IPT_START1 && field.type() <= IPT_START10)
		return u32(field.type()) - u32(IPT_START1);
	if (field.type() >= IPT_COIN1 && field.type() <= IPT_COIN12)
		return u32(field.type()) - u32(IPT_COIN1);
	return field.player();
}

static void build_input_map(kaillera_next_adapter::impl &adapter)
{
	if (adapter.input_map_built)
		return;

	for (auto &port : adapter.machine.ioport().ports())
	{
		for (ioport_field &field : port.second->fields())
		{
			u8 bit = bit_for_field(field);
			if (!bit)
				continue;
			if (field.is_analog())
				continue;
			adapter.input_map.push_back(mapped_input_field{&field, player_for_field(field), bit});
		}
	}

	adapter.input_map_built = true;
	if (adapter.trace)
		osd_printf_info("Kaillera Next trace: mapped_input_fields=%u\n", u32(adapter.input_map.size()));
}

static void sync_programmatic_inputs(kaillera_next_adapter::impl &adapter)
{
	for (auto &port : adapter.machine.ioport().ports())
		port.second->frame_update();
}

static u8 read_default_keyboard(running_machine &machine)
{
	u8 input = 0;
	input_manager &keys = machine.input();

	if (keys.code_pressed(KEYCODE_UP))
		input |= KN_INPUT_UP;
	if (keys.code_pressed(KEYCODE_LEFT))
		input |= KN_INPUT_LEFT;
	if (keys.code_pressed(KEYCODE_RIGHT))
		input |= KN_INPUT_RIGHT;
	if (keys.code_pressed(KEYCODE_DOWN))
		input |= KN_INPUT_DOWN;
	if (keys.code_pressed(KEYCODE_5) || keys.code_pressed(KEYCODE_5_PAD))
		input |= KN_INPUT_COIN;
	if (keys.code_pressed(KEYCODE_1) || keys.code_pressed(KEYCODE_1_PAD))
		input |= KN_INPUT_START;
	if (keys.code_pressed(KEYCODE_LCONTROL) || keys.code_pressed(KEYCODE_RCONTROL) || keys.code_pressed(KEYCODE_Z) || keys.code_pressed(KEYCODE_SPACE))
		input |= KN_INPUT_BUTTON1;
	if (keys.code_pressed(KEYCODE_LALT) || keys.code_pressed(KEYCODE_RALT) || keys.code_pressed(KEYCODE_X))
		input |= KN_INPUT_BUTTON2;

	return input;
}

static u8 read_local_input(kaillera_next_adapter::impl &adapter)
{
	build_input_map(adapter);

	u8 input = 0;
	u32 const source_player = env_u32("KN_MAME_LOCAL_CONTROL_PLAYER", 0);
	for (mapped_input_field &mapped : adapter.input_map)
	{
		if (mapped.player == source_player && field_pressed(adapter.machine, mapped.field))
			input |= mapped.bit;
	}
	input |= read_default_keyboard(adapter.machine);
	return input;
}

static void apply_mapped_inputs(kaillera_next_adapter::impl &adapter, const KnInput *players, u32 player_count)
{
	bool apply_to_mame = env_enabled("KN_MAME_APPLY_INPUT");
	build_input_map(adapter);
	if (apply_to_mame && adapter.input_map.empty())
	{
		if (!adapter.warned_missing_ports)
		{
			osd_printf_error("Kaillera Next: no mappable digital input fields are available; skipping input injection\n");
			adapter.warned_missing_ports = true;
		}
		return;
	}

	for (u32 player = 0; player < std::min<u32>(player_count, 2); player++)
	{
		u8 input = 0;
		if (players && players[player].bytes && players[player].len > 0)
			input = players[player].bytes[0];
		if (input)
			adapter.nonneutral_input_count++;
		if (adapter.trace && player < 2 && input != adapter.last_applied_input[player])
		{
			osd_printf_info("Kaillera Next trace: apply_input frame=%u player=%u input=%02x machine=%s\n",
					adapter.frame_count, player, input, adapter.machine.system().name);
			adapter.last_applied_input[player] = input;
		}
	}

	if (apply_to_mame)
	{
		std::array<u8, 2> inputs = {};
		for (u32 player = 0; player < std::min<u32>(player_count, 2); player++)
		{
			if (players && players[player].bytes && players[player].len > 0)
				inputs[player] = players[player].bytes[0];
		}

		for (mapped_input_field &mapped : adapter.input_map)
		{
			u8 input = mapped.player < inputs.size() ? inputs[mapped.player] : 0;
			set_field(mapped.field, input & mapped.bit);
		}
		sync_programmatic_inputs(adapter);
	}
	adapter.injected_frame_count++;
}

static KnResult KN_CALL kn_mame_poll_local_input(void *user, u32 input_frame, KnMutableInput *out_input)
{
	auto *adapter = static_cast<kaillera_next_adapter::impl *>(user);
	if (!adapter || !out_input || !out_input->bytes || out_input->cap == 0)
		return KN_ERR_INVALID_ARGUMENT;

	std::memset(out_input->bytes, 0, out_input->cap);
	out_input->len = std::min<u32>(out_input->cap, adapter->input_size);
	if (out_input->len > 0 && env_enabled("KN_MAME_LOCAL_INPUT"))
	{
		u8 input = read_local_input(*adapter) | scripted_start_input(input_frame, adapter->player_id);
		if (adapter->trace && input != adapter->last_local_input)
			osd_printf_info("Kaillera Next trace: local_input frame=%u player=%u input=%02x\n", input_frame, adapter->player_id, input);
		out_input->bytes[0] = input;
		adapter->last_local_input = out_input->bytes[0];
	}
	else if (out_input->len > 0 && env_enabled("KN_MAME_SCRIPT_INPUT"))
	{
		out_input->bytes[0] = scripted_pacman_input(input_frame, adapter->player_id);
		adapter->last_local_input = out_input->bytes[0];
	}
	return KN_OK;
}

static KnResult KN_CALL kn_mame_save_state(void *user, u32 frame)
{
	auto *adapter = static_cast<kaillera_next_adapter::impl *>(user);
	if (!adapter)
		return KN_ERR_INVALID_ARGUMENT;
	if (adapter->machine.scheduled_event_pending())
		return KN_OK;

	if (adapter->trace && adapter->save_count < 5)
		osd_printf_info("Kaillera Next trace: save_state frame=%u\n", frame);

	int size = ram_state::get_size(adapter->machine.save());
	if (size <= 0)
		return KN_ERR_CALLBACK;

	std::vector<u8> bytes(size);
	if (adapter->machine.save().write_buffer(bytes.data(), bytes.size()) != STATERR_NONE)
		return KN_ERR_CALLBACK;

	adapter->last_snapshot_size = u32(bytes.size());
	adapter->snapshots[frame] = std::move(bytes);
	adapter->save_count++;
	if (adapter->trace && adapter->save_count < 6)
		osd_printf_info("Kaillera Next trace: save_state done frame=%u size=%u\n", frame, adapter->last_snapshot_size);
	return KN_OK;
}

static KnResult KN_CALL kn_mame_load_state(void *user, u32 frame)
{
	auto *adapter = static_cast<kaillera_next_adapter::impl *>(user);
	if (!adapter)
		return KN_ERR_INVALID_ARGUMENT;
	if (adapter->machine.scheduled_event_pending())
		return KN_OK;

	auto found = adapter->snapshots.find(frame);
	if (found == adapter->snapshots.end())
		return KN_ERR_CALLBACK;

	if (adapter->trace)
		osd_printf_info("Kaillera Next trace: load_state frame=%u size=%u load_count=%u current_mame_frames=%u\n", frame, u32(found->second.size()), adapter->load_count, adapter->frame_count);

	if (adapter->machine.save().read_buffer(found->second.data(), found->second.size()) != STATERR_NONE)
		return KN_ERR_CALLBACK;

	adapter->load_count++;
	if (adapter->trace)
		osd_printf_info("Kaillera Next trace: load_state done frame=%u load_count=%u\n", frame, adapter->load_count);
	return KN_OK;
}

static void KN_CALL kn_mame_discard_states_before(void *user, u32 frame)
{
	auto *adapter = static_cast<kaillera_next_adapter::impl *>(user);
	if (!adapter)
		return;

	for (auto it = adapter->snapshots.begin(); it != adapter->snapshots.end(); )
	{
		if (it->first < frame)
			it = adapter->snapshots.erase(it);
		else
			++it;
	}
}

static KnResult KN_CALL kn_mame_advance_frame(void *user, u32 frame, const KnInput *players, u32 player_count)
{
	auto *adapter = static_cast<kaillera_next_adapter::impl *>(user);
	if (!adapter)
		return KN_ERR_INVALID_ARGUMENT;
	if (adapter->machine.scheduled_event_pending())
		return KN_OK;

	if (adapter->trace && adapter->load_count > 0)
		osd_printf_info("Kaillera Next trace: advance_frame frame=%u before mame_frames=%u loads=%u\n", frame, adapter->frame_count, adapter->load_count);

	apply_mapped_inputs(*adapter, players, player_count);

	adapter->in_advance = true;
	u32 target_frame = adapter->frame_count + 1;
	u32 guard = 0;
	u32 guard_limit = env_u32("KN_MAME_ADVANCE_GUARD", 1000000);
	while (adapter->frame_count < target_frame && !adapter->machine.scheduled_event_pending())
	{
		adapter->machine.scheduler().timeslice();
		if (++guard > guard_limit)
		{
			if (adapter->trace)
				osd_printf_info("Kaillera Next trace: advance_frame guard exhausted frame=%u current_mame_frames=%u target_mame_frames=%u guard=%u\n",
						frame, adapter->frame_count, target_frame, guard);
			adapter->in_advance = false;
			return KN_ERR_CALLBACK;
		}
	}
	adapter->in_advance = false;
	adapter->advance_count++;
	if (adapter->trace && adapter->load_count > 0)
		osd_printf_info("Kaillera Next trace: advance_frame done frame=%u after mame_frames=%u advances=%u\n", frame, adapter->frame_count, adapter->advance_count);

	(void)frame;
	return KN_OK;
}

static u64 KN_CALL kn_mame_state_hash(void *user)
{
	auto *adapter = static_cast<kaillera_next_adapter::impl *>(user);
	if (!adapter)
		return 0;

	int size = ram_state::get_size(adapter->machine.save());
	if (size <= 0)
		return 0;

	std::vector<u8> bytes(size);
	if (adapter->machine.save().write_buffer(bytes.data(), bytes.size()) != STATERR_NONE)
		return 0;

	return fnv1a64(bytes.data(), bytes.size());
}

std::unique_ptr<kaillera_next_adapter> kaillera_next_adapter::create(running_machine &machine)
{
	if (!env_enabled("KN_MAME"))
		return nullptr;

	auto adapter = std::make_unique<kaillera_next_adapter>(machine);
	if (!adapter->initialize())
		return nullptr;
	return adapter;
}

kaillera_next_adapter::kaillera_next_adapter(running_machine &machine) :
	m_impl(std::make_unique<impl>(machine))
{
}

kaillera_next_adapter::~kaillera_next_adapter()
{
	on_exit();
	if (m_impl->client && m_impl->kn_client_destroy)
		m_impl->kn_client_destroy(m_impl->client);
	m_impl->client = nullptr;

	if (m_impl->library)
		dlclose(m_impl->library);
	m_impl->library = nullptr;
}

bool kaillera_next_adapter::initialize()
{
	const char *library_path = env_value("KN_SDK_LIB", "target/debug/libkaillera_next.so");
	m_impl->library = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
	if (!m_impl->library)
	{
		osd_printf_error("Kaillera Next: failed to load %s: %s\n", library_path, dlerror());
		return false;
	}

	if (!load_symbol(m_impl->library, "kn_client_create", m_impl->kn_client_create) ||
		!load_symbol(m_impl->library, "kn_client_destroy", m_impl->kn_client_destroy) ||
		!load_symbol(m_impl->library, "kn_client_tick", m_impl->kn_client_tick) ||
		!load_symbol(m_impl->library, "kn_client_poll_network", m_impl->kn_client_poll_network) ||
		!load_symbol(m_impl->library, "kn_client_get_metrics", m_impl->kn_client_get_metrics) ||
		!load_symbol(m_impl->library, "kn_client_leave", m_impl->kn_client_leave))
	{
		return false;
	}

	const char *server = env_value("KN_SERVER", "");
	m_impl->networked = server[0] != 0;
	const char *session = env_value("KN_SESSION", "mame-bublbobl");
	u32 default_players = server[0] ? 2 : 1;
	u32 input_size = env_u32("KN_INPUT_SIZE", 1);
	u32 player_id = env_u32("KN_PLAYER_ID", 0);
	u32 player_count = env_u32("KN_PLAYERS", default_players);

	m_impl->player_id = player_id;
	m_impl->player_count = player_count;
	m_impl->input_size = input_size;
	m_impl->trace = env_enabled("KN_MAME_TRACE");

	KnCallbacks callbacks = {};
	callbacks.user = m_impl.get();
	callbacks.poll_local_input = kn_mame_poll_local_input;
	callbacks.save_state = kn_mame_save_state;
	callbacks.load_state = kn_mame_load_state;
	callbacks.discard_states_before = kn_mame_discard_states_before;
	callbacks.advance_frame = kn_mame_advance_frame;
	if (std::strcmp(env_value("KN_MAME_STATE_HASH", "1"), "0") != 0)
		callbacks.state_hash = kn_mame_state_hash;

	KnConfig config = {};
	config.api_version = KN_API_VERSION;
	config.server_addr = server;
	config.session_name = session;
	config.player_id = player_id;
	config.player_count = player_count;
	config.input_size = input_size;
	config.input_delay_frames = env_u32("KN_INPUT_DELAY", 0);
	config.max_rollback_frames = env_u32("KN_MAX_ROLLBACK", 120);
	config.max_prediction_frames = env_u32("KN_MAX_PREDICTION", server[0] ? 20 : 0);
	config.net_profile.delay_ms = env_u32("KN_DELAY_MS", 0);
	config.net_profile.jitter_ms = env_u32("KN_JITTER_MS", 0);
	config.net_profile.loss_percent = env_u32("KN_LOSS_PERCENT", 0);

	KnResult result = m_impl->kn_client_create(&config, &callbacks, &m_impl->client);
	if (result != KN_OK)
	{
		osd_printf_error("Kaillera Next: kn_client_create failed result=%d\n", int(result));
		return false;
	}

	m_impl->initialized = true;
	osd_printf_info(
			"Kaillera Next: enabled server=%s session=%s player=%u/%u input_size=%u sdk=%s\n",
			server[0] ? server : "(local)",
			session,
			config.player_id,
			config.player_count,
			config.input_size,
			library_path);
	return true;
}

bool kaillera_next_adapter::tick()
{
	if (!m_impl->initialized || !m_impl->client)
		return false;
	if (m_impl->machine.scheduled_event_pending())
		return false;

	KnResult result = m_impl->kn_client_tick(m_impl->client);
	if (result != KN_OK)
	{
		osd_printf_error("Kaillera Next: tick failed result=%d\n", int(result));
		m_impl->machine.schedule_exit();
	}
	else if (m_impl->networked)
	{
		KnMetrics metrics = {};
		if (m_impl->kn_client_get_metrics(m_impl->client, &metrics) == KN_OK)
		{
			u32 frame_ms = env_u32("KN_MAME_FRAME_MS", 0);
			if (metrics.current_frame == 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(frame_ms > 0 ? frame_ms : 1));

			if (frame_ms > 0 && metrics.current_frame > m_impl->last_paced_frame)
			{
				auto now = std::chrono::steady_clock::now();
				if (m_impl->next_frame_at.time_since_epoch().count() == 0)
					m_impl->next_frame_at = now;
				u32 advanced = metrics.current_frame - m_impl->last_paced_frame;
				m_impl->next_frame_at += std::chrono::milliseconds(u64(frame_ms) * advanced);
				if (now < m_impl->next_frame_at)
					std::this_thread::sleep_until(m_impl->next_frame_at);
				else if (now - m_impl->next_frame_at > std::chrono::milliseconds(250))
					m_impl->next_frame_at = now;
				m_impl->last_paced_frame = metrics.current_frame;
			}
		}
	}
	return true;
}

void kaillera_next_adapter::frame_done()
{
	m_impl->frame_count++;
}

void kaillera_next_adapter::on_exit()
{
	if (!m_impl->initialized || !m_impl->client || m_impl->reported_exit)
		return;

	KnMetrics metrics = {};
	if (m_impl->networked)
	{
		u32 max_polls = env_u32("KN_SETTLE_POLLS", 1000);
		for (u32 poll = 0; poll < max_polls; poll++)
		{
			if (m_impl->kn_client_get_metrics(m_impl->client, &metrics) != KN_OK)
				break;
			if (metrics.confirmed_frame_count >= metrics.current_frame)
			{
				if (m_impl->trace)
					osd_printf_info("Kaillera Next trace: settle complete poll=%u confirmed=%u current=%u\n", poll, metrics.confirmed_frame_count, metrics.current_frame);
				break;
			}
			KnResult poll_result = m_impl->kn_client_poll_network(m_impl->client);
			if (poll_result != KN_OK)
			{
				if (m_impl->trace)
					osd_printf_info("Kaillera Next trace: settle poll failed poll=%u result=%d confirmed=%u current=%u\n", poll, int(poll_result), metrics.confirmed_frame_count, metrics.current_frame);
				break;
			}
			if (m_impl->trace && (poll % 100) == 0)
				osd_printf_info("Kaillera Next trace: settle poll=%u confirmed=%u current=%u\n", poll, metrics.confirmed_frame_count, metrics.current_frame);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	if (m_impl->kn_client_get_metrics(m_impl->client, &metrics) == KN_OK)
	{
		osd_printf_info(
				"kn_mame_adapter summary frames=%u mame_frames=%u confirmed=%u rollbacks=%u max_rollback=%u saves=%u loads=%u advances=%u injected=%u nonneutral=%u last_input=%02x snapshot_bytes=%u input_hash=%016llx state_hash=%016llx missing=%u nacks=%u\n",
				metrics.current_frame,
				m_impl->frame_count,
				metrics.confirmed_frame_count,
				metrics.rollback_count,
				metrics.max_rollback_frames,
				m_impl->save_count,
				m_impl->load_count,
				m_impl->advance_count,
				m_impl->injected_frame_count,
				m_impl->nonneutral_input_count,
				m_impl->last_local_input,
				m_impl->last_snapshot_size,
				(unsigned long long)metrics.confirmed_input_hash,
				(unsigned long long)metrics.current_state_hash,
				metrics.confirmed_missing_frames,
				metrics.confirmed_nack_sent);
	}

	m_impl->kn_client_leave(m_impl->client);
	m_impl->reported_exit = true;
}
