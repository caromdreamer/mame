// license:BSD-3-Clause

#include "emu.h"
#include "main.h"
#include "kaileron_adapter.h"
#include "kaileron_frame_runner.h"
#include "kaileron_state_store.h"

#include "kaileron.h"

#include "osdepend.h"
#include "screen.h"
#include "uiinput.h"
#include "ui/uimain.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

std::string g_kn_mame_status_overlay;
std::string g_kn_mame_chat_overlay;
std::string g_kn_mame_chat_input_overlay;
bool g_kn_mame_chat_active = false;
std::u32string g_kn_mame_chat_pending_chars;

namespace {

using kn_config_init_fn = KnResult (*)(KnConfig *);
using kn_callbacks_init_fn = KnResult (*)(KnCallbacks *, void *);
using kn_playback_status_init_fn = KnResult (*)(KnPlaybackStatus *);
using kn_host_session_create_fn = KnResult (*)(const KnConfig *, const KnCallbacks *, KnHostSession **);
using kn_host_session_destroy_fn = void (*)(KnHostSession *);
using kn_host_session_tick_fn = KnResult (*)(KnHostSession *);
using kn_host_session_get_metrics_fn = KnResult (*)(KnHostSession *, KnMetrics *);
using kn_host_session_get_playback_status_fn = KnResult (*)(KnHostSession *, KnPlaybackStatus *);
using kn_host_session_leave_fn = void (*)(KnHostSession *);

#if defined(_WIN32)
using sdk_library_handle = HMODULE;
#else
using sdk_library_handle = void *;
#endif

// Common digital profile, matching the old Kaillera play-value order for the
// first 15 bits. Later slots are deterministic Kaileron extensions.
constexpr u32 KN_SLOT_BUTTON_BASE = 0;
constexpr u32 KN_SLOT_UP = 7;
constexpr u32 KN_SLOT_DOWN = 8;
constexpr u32 KN_SLOT_LEFT = 9;
constexpr u32 KN_SLOT_RIGHT = 10;
constexpr u32 KN_SLOT_COIN = 11;
constexpr u32 KN_SLOT_START = 12;
constexpr u32 KN_SLOT_SERVICE_MODE = 13;
constexpr u32 KN_SLOT_SERVICE1 = 14;
constexpr u32 KN_SLOT_BUTTON_EXTENSION_BASE = 16;
constexpr u32 KN_SLOT_SERVICE_EXTENSION_BASE = KN_SLOT_BUTTON_EXTENSION_BASE + 9;

enum class socd_mode
{
	off,
	neutral,
	up_priority,
};

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

u64 env_u64(const char *name, u64 fallback)
{
	const char *value = std::getenv(name);
	if (!value || !value[0])
		return fallback;

	char *end = nullptr;
	unsigned long long parsed = std::strtoull(value, &end, 10);
	if (end && *end)
		return fallback;

	return u64(parsed);
}

bool parse_u32(const char *value, u32 &out)
{
	if (!value || !value[0])
		return false;

	char *end = nullptr;
	unsigned long parsed = std::strtoul(value, &end, 10);
	if ((end && *end) || parsed > 0xffffffffUL)
		return false;

	out = u32(parsed);
	return true;
}

bool env_enabled(const char *name)
{
	const char *value = std::getenv(name);
	return value && value[0] && std::strcmp(value, "0");
}

socd_mode parse_socd_mode(const char *value)
{
	if (!value || !value[0] ||
			!std::strcmp(value, "up_priority") ||
			!std::strcmp(value, "up") ||
			!std::strcmp(value, "up_wins"))
		return socd_mode::up_priority;
	if (!std::strcmp(value, "neutral"))
		return socd_mode::neutral;
	if (!std::strcmp(value, "off") || !std::strcmp(value, "none") || !std::strcmp(value, "raw"))
		return socd_mode::off;

	osd_printf_error("Kaileron: unsupported KN_SOCD_MODE=%s; using up_priority\n", value);
	return socd_mode::up_priority;
}

const char *socd_mode_name(socd_mode mode)
{
	switch (mode)
	{
	case socd_mode::off:
		return "off";
	case socd_mode::neutral:
		return "neutral";
	case socd_mode::up_priority:
	default:
		return "up_priority";
	}
}

void show_kaileron_status(running_machine &machine, const std::string &message)
{
	osd_printf_info("%s\n", message.c_str());
	if (!env_enabled("KN_MAME_STATUS"))
	{
		g_kn_mame_status_overlay.clear();
		return;
	}

	g_kn_mame_status_overlay = message;
	machine.popmessage("%s", message.c_str());
}

static void input_slot(u32 slot, u32 &byte, u8 &bit)
{
	byte = slot / 8;
	bit = u8(1U << (slot % 8));
}

static void set_input_slot(u8 *bytes, u32 len, u32 slot)
{
	u32 byte = 0;
	u8 bit = 0;
	input_slot(slot, byte, bit);
	if (bytes && byte < len)
		bytes[byte] |= bit;
}

static bool input_slot_pressed(const u8 *bytes, u32 len, u32 slot)
{
	u32 byte = 0;
	u8 bit = 0;
	input_slot(slot, byte, bit);
	return bytes && byte < len && (bytes[byte] & bit);
}

static void clear_input_slot(u8 *bytes, u32 len, u32 slot)
{
	u32 byte = 0;
	u8 bit = 0;
	input_slot(slot, byte, bit);
	if (bytes && byte < len)
		bytes[byte] &= u8(~bit);
}

static u32 button_slot(u32 button)
{
	return button < 7 ? KN_SLOT_BUTTON_BASE + button : KN_SLOT_BUTTON_EXTENSION_BASE + (button - 7);
}

static void scripted_digital_input(u8 *bytes, u32 len, u32 frame, u32 player)
{
	const u32 phase = (frame / 24 + player) % 4;
	switch (phase)
	{
	case 0:
		set_input_slot(bytes, len, KN_SLOT_RIGHT);
		set_input_slot(bytes, len, button_slot(0));
		break;
	case 1:
		set_input_slot(bytes, len, KN_SLOT_DOWN);
		set_input_slot(bytes, len, button_slot(1));
		break;
	case 2:
		set_input_slot(bytes, len, KN_SLOT_LEFT);
		set_input_slot(bytes, len, button_slot(0));
		break;
	default:
		set_input_slot(bytes, len, KN_SLOT_UP);
		break;
	}
}

const char *default_sdk_library_path()
{
#if defined(_WIN32)
	return "kaileron.dll";
#elif defined(__APPLE__)
	return "target/debug/libkaileron.dylib";
#else
	return "target/debug/libkaileron.so";
#endif
}

sdk_library_handle open_sdk_library(const char *path, std::string &error)
{
#if defined(_WIN32)
	sdk_library_handle library = LoadLibraryA(path);
	if (!library)
		error = "Windows error " + std::to_string(GetLastError());
	return library;
#else
	sdk_library_handle library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!library)
		error = dlerror();
	return library;
#endif
}

void close_sdk_library(sdk_library_handle library)
{
	if (!library)
		return;
#if defined(_WIN32)
	FreeLibrary(library);
#else
	dlclose(library);
#endif
}

template <typename T>
bool load_symbol(sdk_library_handle library, const char *name, T &out)
{
#if defined(_WIN32)
	out = reinterpret_cast<T>(GetProcAddress(library, name));
#else
	out = reinterpret_cast<T>(dlsym(library, name));
#endif
	if (!out)
	{
		osd_printf_error("Kaileron: missing SDK symbol %s\n", name);
		return false;
	}
	return true;
}

struct mapped_input_field
{
	ioport_field *field;
	u32 player;
	u32 byte;
	u8 bit;
	bool owner_only;
};

struct chat_overlay_message
{
	std::string text;
	std::chrono::steady_clock::time_point expires_at;
};

} // namespace

struct kaileron_adapter::impl
{
	explicit impl(running_machine &machine) :
		machine(machine),
		frame_runner(machine),
		state_store(machine)
	{
	}

	running_machine &machine;
	kaileron_frame_runner frame_runner;
	kaileron_state_store state_store;
	sdk_library_handle library = nullptr;
	KnHostSession *session = nullptr;
	kn_config_init_fn kn_config_init = nullptr;
	kn_callbacks_init_fn kn_callbacks_init = nullptr;
	kn_playback_status_init_fn kn_playback_status_init = nullptr;
	kn_host_session_create_fn kn_host_session_create = nullptr;
	kn_host_session_destroy_fn kn_host_session_destroy = nullptr;
	kn_host_session_tick_fn kn_host_session_tick = nullptr;
	kn_host_session_get_metrics_fn kn_host_session_get_metrics = nullptr;
	kn_host_session_get_playback_status_fn kn_host_session_get_playback_status = nullptr;
	kn_host_session_leave_fn kn_host_session_leave = nullptr;
	u32 player_id = 0;
	u32 player_count = 1;
	u64 spectator_id = 0;
	u32 input_size = 1;
	u32 pace_sleep_count = 0;
	u32 injected_frame_count = 0;
	u32 nonneutral_input_count = 0;
	u32 frame_duration_us = 0;
	time_t fixed_rtc_base_time = 0;
	u64 pace_sleep_time_us = 0;
	u8 last_local_input = 0;
	u8 last_applied_input[2] = {0xff, 0xff};
	std::chrono::steady_clock::time_point last_status_at = {};
	std::chrono::steady_clock::time_point last_chat_poll_at = {};
	std::vector<mapped_input_field> input_map;
	std::vector<std::vector<u8>> presentation_inputs;
	std::vector<u8> remote_bootstrap_buffer;
	std::string last_status_text;
	std::string chat_inbox_path;
	std::string chat_outbox_path;
	std::string determinism_dir;
	std::string determinism_role;
	std::string chat_text;
	std::deque<chat_overlay_message> chat_messages;
	u64 last_chat_id = 0;
	bool warned_missing_ports = false;
	bool input_map_built = false;
	bool trace = false;
	bool initialized = false;
	bool chat_active = false;
	bool chat_hotkey_down = false;
	bool chat_enter_ready = false;
	bool reported_exit = false;
	bool in_advance = false;
	bool networked = false;
	bool spectator = false;
	bool original_throttled = true;
	bool original_ui_mute = false;
	u32 original_speed_factor = 1000;
	u32 sdk_playback_speed_factor = 1000;
	u32 sdk_render_interval = 1;
	u32 sdk_pace_delay_us = 0;
	u32 requested_runahead_frames = 0;
	u32 runahead_frames = 0;
	u32 presentation_sdk_frame = 0;
	socd_mode local_socd_mode = socd_mode::up_priority;
	bool sdk_playback_override = false;
	bool rollback_replay_active = false;
	bool presentation_ready = false;
	bool verify_presentation_restore = false;
	bool show_playback_status = false;
	bool show_rollback_stats = false;
	bool fixed_rtc_enabled = false;
	bool remote_bootstrap_enabled = false;
	bool determinism_bootstrap_ready = false;
	std::array<bool, 3> determinism_checkpoints_written = {false, false, false};
};

enum class determinism_bootstrap_result
{
	ready,
	waiting,
	failed
};

constexpr std::array<u32, 3> DETERMINISM_CHECKPOINT_FRAMES = {60, 300, 1200};
// Avoid serializing MAME before device startup and postload paths are safe.
constexpr u32 REMOTE_BOOTSTRAP_WARMUP_FRAMES = 60;

static std::string determinism_path(
		kaileron_adapter::impl const &adapter,
		std::string const &filename)
{
	return (std::filesystem::path(adapter.determinism_dir) / filename).string();
}

static std::string determinism_checkpoint_stem(
		kaileron_adapter::impl const &adapter,
		u32 frame)
{
	return adapter.determinism_role + "-frame" +
			[] (u32 value) {
				std::ostringstream output;
				output << std::setw(6) << std::setfill('0') << value;
				return output.str();
			}(frame);
}

static bool write_determinism_current_checkpoint(
		kaileron_adapter::impl &adapter,
		u32 frame)
{
	std::string const stem = determinism_checkpoint_stem(adapter, frame);
	u64 state_hash = 0;
	if (!adapter.state_store.export_current(
			determinism_path(adapter, stem + ".bin"), state_hash))
		return false;
	if (!adapter.state_store.write_current_manifest(
			determinism_path(adapter, stem + ".tsv"), frame))
		return false;
	osd_printf_info(
			"Kaileron determinism: checkpoint role=%s frame=%u state_hash=%016llx\n",
			adapter.determinism_role.c_str(),
			frame,
			(unsigned long long)state_hash);
	return true;
}

static bool write_determinism_confirmed_checkpoint(
		kaileron_adapter::impl &adapter,
		u32 frame)
{
	std::string const stem = determinism_checkpoint_stem(adapter, frame);
	u64 state_hash = 0;
	if (!adapter.state_store.export_snapshot(
			frame, determinism_path(adapter, stem + ".bin"), state_hash))
		return false;
	if (!adapter.state_store.write_snapshot_manifest(
			frame, determinism_path(adapter, stem + ".tsv")))
		return false;
	osd_printf_info(
			"Kaileron determinism: confirmed checkpoint role=%s frame=%u state_hash=%016llx\n",
			adapter.determinism_role.c_str(),
			frame,
			(unsigned long long)state_hash);
	return true;
}

static determinism_bootstrap_result prepare_determinism_bootstrap(
		kaileron_adapter::impl &adapter)
{
	if (adapter.determinism_dir.empty() || adapter.determinism_bootstrap_ready)
		return determinism_bootstrap_result::ready;
	if (adapter.remote_bootstrap_enabled)
		return determinism_bootstrap_result::ready;

	std::string const bootstrap_path = determinism_path(adapter, "bootstrap.bin");
	u64 state_hash = 0;
	bool const authority = !adapter.spectator && adapter.player_id == 0;
	if (authority)
	{
		if (!adapter.state_store.export_current(bootstrap_path, state_hash))
			return determinism_bootstrap_result::failed;
	}
	else if (!std::filesystem::exists(bootstrap_path))
	{
		return determinism_bootstrap_result::waiting;
	}
	// Every participant, including the authority that wrote the checkpoint,
	// must traverse MAME's postload callbacks before frame zero.
	if (!adapter.state_store.import_current(bootstrap_path, state_hash))
		return determinism_bootstrap_result::failed;
	if (!write_determinism_current_checkpoint(adapter, 0))
		return determinism_bootstrap_result::failed;

	adapter.determinism_bootstrap_ready = true;
	osd_printf_info(
			"Kaileron determinism: bootstrap role=%s source=%s state_hash=%016llx\n",
			adapter.determinism_role.c_str(),
			authority ? "authority_reload" : "import",
			(unsigned long long)state_hash);
	return determinism_bootstrap_result::ready;
}

static void apply_playback_control(kaileron_adapter::impl &adapter, const KnPlaybackControl &control)
{
	adapter.sdk_pace_delay_us = control.pace_delay_us;
	if (!adapter.spectator)
		return;

	u32 speed_percent = control.target_speed_percent ? control.target_speed_percent : 100;
	speed_percent = std::clamp<u32>(speed_percent, 100, 12800);
	u32 const speed_factor = speed_percent * 10;
	bool const enabled = speed_factor != 1000;
	if (enabled)
	{
		if (!adapter.sdk_playback_override)
		{
			adapter.original_throttled = adapter.machine.video().throttled();
			adapter.original_ui_mute = adapter.machine.sound().ui_mute();
			adapter.original_speed_factor = adapter.machine.video().speed_factor();
		}
		adapter.machine.video().set_fastforward(false);
		adapter.machine.video().set_throttled(false);
		adapter.machine.video().set_speed_factor(adapter.original_speed_factor);
		adapter.machine.sound().ui_mute(adapter.original_ui_mute || control.mute_audio);
		adapter.sdk_playback_speed_factor = speed_factor;
		adapter.sdk_render_interval = std::max<u32>(control.render_interval, 1);
	}
	else if (adapter.sdk_playback_override)
	{
		adapter.machine.video().set_fastforward(false);
		adapter.machine.video().set_throttled(adapter.original_throttled);
		adapter.machine.video().set_speed_factor(adapter.original_speed_factor);
		adapter.machine.sound().ui_mute(adapter.original_ui_mute);
		adapter.sdk_playback_speed_factor = 1000;
		adapter.sdk_render_interval = 1;
	}
	adapter.sdk_playback_override = enabled;
}

static void synchronize_fixed_rtc(kaileron_adapter::impl &adapter, u32 completed_frame)
{
	if (!adapter.fixed_rtc_enabled || adapter.frame_duration_us == 0)
		return;

	u64 const elapsed_seconds =
			(u64(completed_frame) * adapter.frame_duration_us) / 1'000'000ULL;
	adapter.machine.set_rtc_datetime(system_time(
			adapter.fixed_rtc_base_time + time_t(elapsed_seconds)));
}

static void set_live_field(ioport_field *field, bool pressed)
{
	if (!field)
		return;

	if (pressed)
		field->set_value(1);
	else
		field->clear_value();

	ioport_value &digital = field->port().live().digital;
	if (pressed)
		digital |= field->mask();
	else
		digital &= ~field->mask();
}

static std::string normalized_chat_line(const std::string &value)
{
	std::string out = value;
	for (char &ch : out)
		if (ch == '\r' || ch == '\n' || ch == '\t')
			ch = ' ';
	return out;
}

static void append_utf8(std::string &out, char32_t ch)
{
	if (ch <= 0x7f)
		out.push_back(char(ch));
	else if (ch <= 0x7ff)
	{
		out.push_back(char(0xc0 | (ch >> 6)));
		out.push_back(char(0x80 | (ch & 0x3f)));
	}
	else if (ch <= 0xffff)
	{
		out.push_back(char(0xe0 | (ch >> 12)));
		out.push_back(char(0x80 | ((ch >> 6) & 0x3f)));
		out.push_back(char(0x80 | (ch & 0x3f)));
	}
	else
	{
		out.push_back(char(0xf0 | (ch >> 18)));
		out.push_back(char(0x80 | ((ch >> 12) & 0x3f)));
		out.push_back(char(0x80 | ((ch >> 6) & 0x3f)));
		out.push_back(char(0x80 | (ch & 0x3f)));
	}
}

static void pop_utf8_codepoint(std::string &text)
{
	if (text.empty())
		return;
	std::size_t pos = text.size() - 1;
	while (pos > 0 && (u8(text[pos]) & 0xc0) == 0x80)
		pos--;
	text.erase(pos);
}

static void write_chat_outbox(const std::string &path, const std::string &body)
{
	if (path.empty() || body.empty())
		return;
	std::ofstream file(path, std::ios::app);
	if (!file)
		return;
	file << normalized_chat_line(body) << '\n';
}

static void refresh_chat_overlay(kaileron_adapter::impl &adapter)
{
	auto const now = std::chrono::steady_clock::now();
	while (!adapter.chat_messages.empty() && adapter.chat_messages.front().expires_at <= now)
		adapter.chat_messages.pop_front();

	std::string overlay;
	for (chat_overlay_message const &message : adapter.chat_messages)
	{
		if (!overlay.empty())
			overlay.push_back('\n');
		overlay += message.text;
	}
	g_kn_mame_chat_overlay = overlay;
}

static void append_chat_overlay(kaileron_adapter::impl &adapter, const std::string &message)
{
	if (message.empty())
		return;

	constexpr std::size_t max_messages = 6;
	auto const now = std::chrono::steady_clock::now();
	adapter.chat_messages.push_back(chat_overlay_message{message, now + std::chrono::seconds(12)});
	while (adapter.chat_messages.size() > max_messages)
		adapter.chat_messages.pop_front();
	refresh_chat_overlay(adapter);
}

static bool key_pressed_once(running_machine &machine, input_item_id item)
{
	return machine.input().code_pressed_once(machine.input().code_from_itemid(item));
}

static bool mapped_raw_input_pressed(running_machine &machine, mapped_input_field const &mapped)
{
	return mapped.field &&
			mapped.field->enabled() &&
			!machine.ui().is_menu_active() &&
			!g_kn_mame_chat_active &&
			machine.input().seq_pressed(mapped.field->seq());
}

static bool mapped_local_input_pressed(running_machine &machine, mapped_input_field const &mapped)
{
	return mapped_raw_input_pressed(machine, mapped);
}

static bool field_mapping(ioport_field &field, u32 &byte, u8 &bit)
{
	switch (field.type())
	{
	case IPT_JOYSTICK_UP:
	case IPT_JOYSTICKLEFT_UP:
	case IPT_JOYSTICKRIGHT_UP:
		input_slot(KN_SLOT_UP, byte, bit);
		return true;
	case IPT_JOYSTICK_LEFT:
	case IPT_JOYSTICKLEFT_LEFT:
	case IPT_JOYSTICKRIGHT_LEFT:
		input_slot(KN_SLOT_LEFT, byte, bit);
		return true;
	case IPT_JOYSTICK_RIGHT:
	case IPT_JOYSTICKLEFT_RIGHT:
	case IPT_JOYSTICKRIGHT_RIGHT:
		input_slot(KN_SLOT_RIGHT, byte, bit);
		return true;
	case IPT_JOYSTICK_DOWN:
	case IPT_JOYSTICKLEFT_DOWN:
	case IPT_JOYSTICKRIGHT_DOWN:
		input_slot(KN_SLOT_DOWN, byte, bit);
		return true;
	case IPT_START:
	case IPT_SELECT:
		input_slot(field.type() == IPT_START ? KN_SLOT_START : KN_SLOT_COIN, byte, bit);
		return true;
	default:
		break;
	}

	if (field.type() >= IPT_COIN1 && field.type() <= IPT_COIN12)
	{
		input_slot(KN_SLOT_COIN, byte, bit);
		return true;
	}
	if (field.type() >= IPT_START1 && field.type() <= IPT_START10)
	{
		input_slot(KN_SLOT_START, byte, bit);
		return true;
	}
	if (field.type() >= IPT_SERVICE1 && field.type() <= IPT_SERVICE4)
	{
		u32 const service = u32(field.type()) - u32(IPT_SERVICE1);
		input_slot(service == 0 ? KN_SLOT_SERVICE1 : KN_SLOT_SERVICE_EXTENSION_BASE + (service - 1), byte, bit);
		return true;
	}
	if (field.type() == IPT_SERVICE)
	{
		input_slot(KN_SLOT_SERVICE_MODE, byte, bit);
		return true;
	}
	if (field.type() >= IPT_BUTTON1 && field.type() <= IPT_BUTTON16)
	{
		u32 const button = u32(field.type()) - u32(IPT_BUTTON1);
		input_slot(button_slot(button), byte, bit);
		return true;
	}
	bit = 0;
	return false;
}

static bool owner_only_input_field(ioport_field &field)
{
	return (field.type() >= IPT_SERVICE1 && field.type() <= IPT_SERVICE4) ||
			field.type() == IPT_SERVICE;
}

static u32 player_for_field(ioport_field &field)
{
	if (field.type() >= IPT_START1 && field.type() <= IPT_START10)
		return u32(field.type()) - u32(IPT_START1);
	if (field.type() >= IPT_COIN1 && field.type() <= IPT_COIN12)
		return u32(field.type()) - u32(IPT_COIN1);
	if (field.type() >= IPT_SERVICE1 && field.type() <= IPT_SERVICE4)
		return 0;
	if (field.type() == IPT_SERVICE)
		return 0;
	return field.player();
}

static void build_input_map(kaileron_adapter::impl &adapter)
{
	if (adapter.input_map_built)
		return;

	for (auto &port : adapter.machine.ioport().ports())
	{
		for (ioport_field &field : port.second->fields())
		{
			u32 byte = 0;
			u8 bit = 0;
			if (!field_mapping(field, byte, bit))
				continue;
			if (field.is_analog())
				continue;
			adapter.input_map.push_back(mapped_input_field{&field, player_for_field(field), byte, bit, owner_only_input_field(field)});
		}
	}

	adapter.input_map_built = true;
	if (adapter.trace)
		osd_printf_info("Kaileron trace: mapped_input_fields=%u\n", u32(adapter.input_map.size()));
	if (env_enabled("KN_MAME_DUMP_INPUT_MAP"))
	{
		for (mapped_input_field const &mapped : adapter.input_map)
		{
			std::string const name = mapped.field->name();
			osd_printf_info(
					"Kaileron input map: player=%u byte=%u bit=%02x type=%u name=%s\n",
					mapped.player,
					mapped.byte,
					mapped.bit,
					u32(mapped.field->type()),
					name.c_str());
		}
	}
}

static u32 required_input_size(kaileron_adapter::impl &adapter)
{
	build_input_map(adapter);

	u32 bytes = 1;
	for (mapped_input_field const &mapped : adapter.input_map)
		bytes = std::max<u32>(bytes, mapped.byte + 1);
	return bytes;
}

static bool input_size_env_is_auto()
{
	const char *value = std::getenv("KN_INPUT_SIZE");
	return !value || !value[0] || std::strcmp(value, "auto") == 0;
}

static u32 resolve_input_size(kaileron_adapter::impl &adapter)
{
	u32 const required = required_input_size(adapter);
	const char *value = std::getenv("KN_INPUT_SIZE");
	if (input_size_env_is_auto())
	{
		if (adapter.trace || env_enabled("KN_MAME_DUMP_INPUT_MAP"))
			osd_printf_info("Kaileron: auto input_size=%u from mapped ioports\n", required);
		return required;
	}

	u32 requested = 0;
	if (!parse_u32(value, requested) || requested == 0)
	{
		osd_printf_error("Kaileron: invalid KN_INPUT_SIZE=%s; using auto input_size=%u\n", value, required);
		return required;
	}
	if (requested < required)
	{
		osd_printf_error("Kaileron: KN_INPUT_SIZE=%u is smaller than mapped ioports require %u; raising input_size\n", requested, required);
		return required;
	}
	return requested;
}

static u32 frame_duration_us(running_machine &machine)
{
	screen_device *screen = screen_device_enumerator(machine.root_device()).first();
	if (!screen)
		return 0;

	attoseconds_t const attoseconds = screen->frame_period().as_attoseconds();
	if (attoseconds <= 0)
		return 0;

	u64 const microseconds = u64(attoseconds / ATTOSECONDS_PER_MICROSECOND);
	return microseconds > 0xffffffffULL ? 0 : u32(microseconds);
}

static void set_input_bit(u8 *bytes, u32 len, u32 byte, u8 bit)
{
	if (bytes && byte < len)
		bytes[byte] |= bit;
}

static void apply_socd_cleaning(kaileron_adapter::impl &adapter, u8 *bytes, u32 len)
{
	if (adapter.local_socd_mode == socd_mode::off)
		return;

	if (input_slot_pressed(bytes, len, KN_SLOT_LEFT) && input_slot_pressed(bytes, len, KN_SLOT_RIGHT))
	{
		clear_input_slot(bytes, len, KN_SLOT_LEFT);
		clear_input_slot(bytes, len, KN_SLOT_RIGHT);
	}

	if (input_slot_pressed(bytes, len, KN_SLOT_UP) && input_slot_pressed(bytes, len, KN_SLOT_DOWN))
	{
		if (adapter.local_socd_mode == socd_mode::up_priority)
			clear_input_slot(bytes, len, KN_SLOT_DOWN);
		else
		{
			clear_input_slot(bytes, len, KN_SLOT_UP);
			clear_input_slot(bytes, len, KN_SLOT_DOWN);
		}
	}
}

static void read_local_input(kaileron_adapter::impl &adapter, u8 *bytes, u32 len)
{
	build_input_map(adapter);

	u32 const source_player = env_u32("KN_MAME_LOCAL_CONTROL_PLAYER", 0);
	for (mapped_input_field &mapped : adapter.input_map)
	{
		if (mapped.owner_only && adapter.player_id != 0)
			continue;
		if ((mapped.owner_only || mapped.player == source_player) && mapped_local_input_pressed(adapter.machine, mapped))
			set_input_bit(bytes, len, mapped.byte, mapped.bit);
	}
	apply_socd_cleaning(adapter, bytes, len);
}

static bool input_bit_pressed(KnInput const &input, mapped_input_field const &mapped)
{
	return input.bytes && mapped.byte < input.len && (input.bytes[mapped.byte] & mapped.bit);
}

static bool mapped_input_pressed(const KnInput *players, u32 player_count, mapped_input_field const &mapped)
{
	if (!players)
		return false;

	if (mapped.owner_only)
	{
		return player_count > 0 && input_bit_pressed(players[0], mapped);
	}

	return mapped.player < player_count && input_bit_pressed(players[mapped.player], mapped);
}

static std::vector<KnInput> cleaned_player_inputs(kaileron_adapter::impl &adapter, const KnInput *players, u32 player_count, std::vector<std::vector<u8>> &storage)
{
	std::vector<KnInput> cleaned;
	if (!players || player_count == 0)
		return cleaned;

	cleaned.reserve(player_count);
	storage.reserve(player_count);
	for (u32 player = 0; player < player_count; player++)
	{
		KnInput input = players[player];
		if (input.bytes && input.len > 0)
		{
			storage.emplace_back(input.bytes, input.bytes + input.len);
			apply_socd_cleaning(adapter, storage.back().data(), input.len);
			input.bytes = storage.back().data();
		}
		cleaned.push_back(input);
	}
	return cleaned;
}

static void apply_mapped_inputs(
		kaileron_adapter::impl &adapter,
		const KnInput *players,
		u32 player_count,
		bool count_stats)
{
	bool apply_to_mame = env_enabled("KN_MAME_APPLY_INPUT");
	build_input_map(adapter);
	if (apply_to_mame && adapter.input_map.empty())
	{
		if (!adapter.warned_missing_ports)
		{
			osd_printf_error("Kaileron: no mappable digital input fields are available; skipping input injection\n");
			adapter.warned_missing_ports = true;
		}
		return;
	}

	std::vector<std::vector<u8>> cleaned_storage;
	std::vector<KnInput> cleaned_inputs = cleaned_player_inputs(adapter, players, player_count, cleaned_storage);
	const KnInput *canonical_players = cleaned_inputs.empty() ? players : cleaned_inputs.data();

	for (u32 player = 0; player < std::min<u32>(player_count, 2); player++)
	{
		u8 input = 0;
		if (canonical_players && canonical_players[player].bytes && canonical_players[player].len > 0)
			input = canonical_players[player].bytes[0];
		if (count_stats && input)
			adapter.nonneutral_input_count++;
		if (count_stats && adapter.trace && player < 2 && input != adapter.last_applied_input[player])
		{
			osd_printf_info("Kaileron trace: apply_input frame=%u player=%u input=%02x machine=%s\n",
					adapter.frame_runner.completed_frame_count(), player, input, adapter.machine.system().name);
			adapter.last_applied_input[player] = input;
		}
	}

	if (apply_to_mame)
	{
		for (mapped_input_field &mapped : adapter.input_map)
		{
			bool const pressed = mapped_input_pressed(canonical_players, player_count, mapped);
			set_live_field(mapped.field, pressed);
		}
	}
	if (count_stats)
		adapter.injected_frame_count++;
}

static void cache_presentation_inputs(
		kaileron_adapter::impl &adapter,
		u32 frame,
		const KnInput *players,
		u32 player_count)
{
	adapter.presentation_inputs.clear();
	adapter.presentation_inputs.resize(player_count);
	for (u32 player = 0; players && player < player_count; player++)
	{
		if (players[player].bytes && players[player].len > 0)
			adapter.presentation_inputs[player].assign(
					players[player].bytes,
					players[player].bytes + players[player].len);
	}
	adapter.presentation_sdk_frame = frame;
}

static std::vector<KnInput> presentation_input_views(kaileron_adapter::impl const &adapter)
{
	std::vector<KnInput> inputs;
	inputs.reserve(adapter.presentation_inputs.size());
	for (std::vector<u8> const &bytes : adapter.presentation_inputs)
		inputs.push_back(KnInput{bytes.empty() ? nullptr : bytes.data(), u32(bytes.size())});
	return inputs;
}

static bool runahead_eligible(kaileron_adapter::impl const &adapter)
{
	return adapter.runahead_frames > 0 &&
			!adapter.spectator &&
			!adapter.sdk_playback_override &&
			!adapter.rollback_replay_active;
}

static bool run_presentation_runahead(kaileron_adapter::impl &adapter)
{
	if (!adapter.presentation_ready || !runahead_eligible(adapter))
		return true;

	adapter.presentation_ready = false;
	if (adapter.machine.scheduled_event_pending())
		return true;

	if (!adapter.state_store.save_presentation())
		return false;

	std::vector<KnInput> const inputs = presentation_input_views(adapter);
	bool advanced = true;
	for (u32 lookahead = 0; lookahead < adapter.runahead_frames; lookahead++)
	{
		bool const final_frame = lookahead + 1 == adapter.runahead_frames;
		apply_mapped_inputs(adapter, inputs.data(), u32(inputs.size()), false);
		if (!adapter.frame_runner.advance(
				adapter.presentation_sdk_frame,
				kaileron_frame_request::speculative_presentation(final_frame)))
		{
			advanced = false;
			break;
		}
	}

	// Restoration is mandatory even when speculative execution fails.
	if (!adapter.state_store.restore_presentation())
		return false;

	if (adapter.verify_presentation_restore)
	{
		if (!adapter.state_store.verify_presentation_restore())
		{
			u64 const restored_hash = adapter.state_store.current_state_hash();
			osd_printf_error(
					"Kaileron: runahead restore mismatch frame=%u before=%016llx after=%016llx\n",
					adapter.presentation_sdk_frame,
					(unsigned long long)adapter.state_store.presentation_snapshot_hash(),
					(unsigned long long)restored_hash);
			return false;
		}
	}

	return advanced;
}

static KnResult KN_CALL kn_mame_poll_local_input(void *user, u32 input_frame, KnMutableInput *out_input)
{
	auto *adapter = static_cast<kaileron_adapter::impl *>(user);
	if (!adapter || !out_input || !out_input->bytes || out_input->cap == 0)
		return KN_ERR_INVALID_ARGUMENT;

	std::memset(out_input->bytes, 0, out_input->cap);
	out_input->len = std::min<u32>(out_input->cap, adapter->input_size);
	if (out_input->len > 0 && env_enabled("KN_MAME_LOCAL_INPUT"))
	{
		read_local_input(*adapter, out_input->bytes, out_input->len);
		if (adapter->trace && out_input->bytes[0] != adapter->last_local_input)
			osd_printf_info("Kaileron trace: local_input frame=%u player=%u input=%02x\n", input_frame, adapter->player_id, out_input->bytes[0]);
		adapter->last_local_input = out_input->bytes[0];
	}
	else if (out_input->len > 0 && env_enabled("KN_MAME_SCRIPT_INPUT"))
	{
		u32 const stop_frame = env_u32("KN_MAME_SCRIPT_INPUT_STOP_FRAME", std::numeric_limits<u32>::max());
		if (input_frame < stop_frame)
			scripted_digital_input(out_input->bytes, out_input->len, input_frame, adapter->player_id);
		adapter->last_local_input = out_input->bytes[0];
	}
	return KN_OK;
}

static KnResult KN_CALL kn_mame_save_state(void *user, u32 frame)
{
	auto *adapter = static_cast<kaileron_adapter::impl *>(user);
	if (!adapter)
		return KN_ERR_INVALID_ARGUMENT;
	if (adapter->machine.scheduled_event_pending())
		return KN_OK;

	if (!adapter->state_store.save(frame))
		return KN_ERR_CALLBACK;
	return KN_OK;
}

static KnResult KN_CALL kn_mame_load_state(void *user, u32 frame)
{
	auto *adapter = static_cast<kaileron_adapter::impl *>(user);
	if (!adapter)
		return KN_ERR_INVALID_ARGUMENT;
	if (adapter->machine.scheduled_event_pending())
		return KN_OK;

	if (!adapter->state_store.load(frame, adapter->frame_runner.completed_frame_count()))
		return KN_ERR_CALLBACK;

	adapter->rollback_replay_active = true;
	return KN_OK;
}

static void KN_CALL kn_mame_discard_states_before(void *user, u32 frame)
{
	auto *adapter = static_cast<kaileron_adapter::impl *>(user);
	if (!adapter)
		return;

	adapter->state_store.discard_before(frame);
}

static KnResult KN_CALL kn_mame_advance_frame(void *user, u32 frame, const KnInput *players, u32 player_count)
{
	auto *adapter = static_cast<kaileron_adapter::impl *>(user);
	if (!adapter)
		return KN_ERR_INVALID_ARGUMENT;
	if (adapter->machine.scheduled_event_pending())
		return KN_OK;

	if (adapter->trace && adapter->state_store.load_count() > 0)
		osd_printf_info("Kaileron trace: advance_frame frame=%u before mame_frames=%u loads=%u\n", frame, adapter->frame_runner.completed_frame_count(), adapter->state_store.load_count());

	bool const skip_catchup_video = adapter->sdk_playback_override &&
			adapter->sdk_render_interval > 1 &&
			(frame % adapter->sdk_render_interval) != 0;
	bool const present_with_runahead = runahead_eligible(*adapter);
	kaileron_frame_request const request = adapter->rollback_replay_active
			? kaileron_frame_request::rollback_resimulation()
			: adapter->sdk_playback_override
					? kaileron_frame_request::spectator_catchup(!skip_catchup_video)
					: kaileron_frame_request::authoritative(!present_with_runahead);

	if (present_with_runahead)
		cache_presentation_inputs(*adapter, frame, players, player_count);
	apply_mapped_inputs(*adapter, players, player_count, true);

	adapter->in_advance = true;
	if (!adapter->frame_runner.advance(frame, request))
	{
		adapter->in_advance = false;
		return KN_ERR_CALLBACK;
	}

	adapter->in_advance = false;
	synchronize_fixed_rtc(*adapter, frame + 1);
	if (present_with_runahead)
		adapter->presentation_ready = true;
	if (adapter->trace && adapter->state_store.load_count() > 0)
		osd_printf_info("Kaileron trace: advance_frame done frame=%u after mame_frames=%u advances=%u\n", frame, adapter->frame_runner.completed_frame_count(), adapter->frame_runner.advance_count());

	(void)frame;
	return KN_OK;
}

static u64 KN_CALL kn_mame_state_hash(void *user)
{
	auto *adapter = static_cast<kaileron_adapter::impl *>(user);
	if (!adapter)
		return 0;

	return adapter->state_store.current_state_hash();
}

static u32 KN_CALL kn_mame_serialized_state_size(void *user)
{
	auto &adapter = *static_cast<kaileron_adapter::impl *>(user);
	u64 state_hash = 0;
	adapter.remote_bootstrap_buffer.clear();
	if (!adapter.state_store.export_current(adapter.remote_bootstrap_buffer, state_hash))
		return 0;
	if (adapter.remote_bootstrap_buffer.size() > std::numeric_limits<u32>::max())
	{
		adapter.remote_bootstrap_buffer.clear();
		return 0;
	}
	return u32(adapter.remote_bootstrap_buffer.size());
}

static KnResult KN_CALL kn_mame_export_serialized_state(void *user, u8 *bytes, u32 len)
{
	auto &adapter = *static_cast<kaileron_adapter::impl *>(user);
	if (!bytes || adapter.remote_bootstrap_buffer.size() != len)
		return KN_ERR_INVALID_ARGUMENT;
	std::memcpy(bytes, adapter.remote_bootstrap_buffer.data(), len);
	adapter.remote_bootstrap_buffer.clear();
	return KN_OK;
}

static KnResult KN_CALL kn_mame_import_serialized_state(void *user, u8 const *bytes, u32 len)
{
	auto &adapter = *static_cast<kaileron_adapter::impl *>(user);
	u64 state_hash = 0;
	adapter.remote_bootstrap_buffer.clear();
	if (!adapter.state_store.import_current(bytes, len, state_hash))
		return KN_ERR_CALLBACK;
	if (!adapter.determinism_dir.empty() &&
			!write_determinism_current_checkpoint(adapter, 0))
		return KN_ERR_CALLBACK;
	adapter.determinism_bootstrap_ready = true;
	osd_printf_info(
			"Kaileron: remote bootstrap imported bytes=%u state_hash=%016llx\n",
			len,
			(unsigned long long)state_hash);
	return KN_OK;
}

static void KN_CALL kn_mame_set_playback_control(void *user, const KnPlaybackControl *control)
{
	auto *adapter = static_cast<kaileron_adapter::impl *>(user);
	if (!adapter || !control)
		return;
	apply_playback_control(*adapter, *control);
}

static const char *lifecycle_event_name(u32 type)
{
	switch (type)
	{
	case KN_LIFECYCLE_SESSION_CREATED:
		return "session_created";
	case KN_LIFECYCLE_SESSION_CONNECTING:
		return "session_connecting";
	case KN_LIFECYCLE_SESSION_WELCOMED:
		return "session_welcomed";
	case KN_LIFECYCLE_SESSION_READY_SENT:
		return "session_ready_sent";
	case KN_LIFECYCLE_SESSION_STARTING:
		return "session_starting";
	case KN_LIFECYCLE_SESSION_STARTED:
		return "session_started";
	case KN_LIFECYCLE_SESSION_LEFT:
		return "session_left";
	case KN_LIFECYCLE_PEER_LEFT:
		return "peer_left";
	case KN_LIFECYCLE_OBSERVER_PREPARING:
		return "observer_preparing";
	case KN_LIFECYCLE_OBSERVER_READY:
		return "observer_ready";
	case KN_LIFECYCLE_SEAT_CLAIM_OFFERED:
		return "seat_claim_offered";
	case KN_LIFECYCLE_SEAT_ACTIVATED:
		return "seat_activated";
	case KN_LIFECYCLE_PEER_JOINED:
		return "peer_joined";
	case KN_LIFECYCLE_SEAT_CLAIM_CANCELLED:
		return "seat_claim_cancelled";
	case KN_LIFECYCLE_ROLLBACK_BEGIN:
		return "rollback_begin";
	case KN_LIFECYCLE_ROLLBACK_END:
		return "rollback_end";
	case KN_LIFECYCLE_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

static void KN_CALL kn_mame_on_lifecycle_event(void *user, const KnLifecycleEvent *event)
{
	auto *adapter = static_cast<kaileron_adapter::impl *>(user);
	if (!adapter || !event)
		return;

	const char *name = lifecycle_event_name(event->type);
	bool const rollback_event = event->type == KN_LIFECYCLE_ROLLBACK_BEGIN ||
			event->type == KN_LIFECYCLE_ROLLBACK_END;
	if (adapter->trace || !rollback_event)
		osd_printf_info("Kaileron lifecycle: %s frame=%u peer=%u reason=%u\n",
				name,
				event->frame,
				event->peer_id,
				event->reason);

	if (event->type == KN_LIFECYCLE_SESSION_STARTED)
		show_kaileron_status(adapter->machine, "Kaileron: session started");
	else if (event->type == KN_LIFECYCLE_ROLLBACK_BEGIN)
		adapter->rollback_replay_active = true;
	else if (event->type == KN_LIFECYCLE_ROLLBACK_END)
		adapter->rollback_replay_active = false;
	else if (event->type == KN_LIFECYCLE_SESSION_WELCOMED)
		show_kaileron_status(adapter->machine, "Kaileron: connected");
	else if (event->type == KN_LIFECYCLE_SESSION_STARTING)
		show_kaileron_status(adapter->machine, "Kaileron: starting session");
	else if (event->type == KN_LIFECYCLE_SEAT_CLAIM_OFFERED)
		show_kaileron_status(adapter->machine, "Kaileron: preparing player " + std::to_string(event->peer_id + 1));
	else if (event->type == KN_LIFECYCLE_SEAT_ACTIVATED)
	{
		KnPlaybackControl live = {};
		live.target_speed_percent = 100;
		live.render_interval = 1;
		apply_playback_control(*adapter, live);
		adapter->player_id = event->peer_id;
		adapter->spectator = false;
		adapter->runahead_frames = adapter->requested_runahead_frames;
		adapter->determinism_role = "p" + std::to_string(adapter->player_id);
		show_kaileron_status(adapter->machine, "Kaileron: joined as player " + std::to_string(adapter->player_id + 1));
	}
	else if (event->type == KN_LIFECYCLE_PEER_JOINED)
		show_kaileron_status(adapter->machine, "Kaileron: player " + std::to_string(event->peer_id + 1) + " joined");
	else if (event->type == KN_LIFECYCLE_SEAT_CLAIM_CANCELLED)
	{
		adapter->spectator = true;
		adapter->runahead_frames = 0;
		show_kaileron_status(adapter->machine, "Kaileron: player join cancelled");
	}
	else if (event->type == KN_LIFECYCLE_PEER_LEFT)
		show_kaileron_status(adapter->machine, "Kaileron: peer left");
	else if (event->type == KN_LIFECYCLE_ERROR &&
			event->reason == KN_LIFECYCLE_ERROR_SERVER_TIMEOUT)
		show_kaileron_status(adapter->machine, "Kaileron: server disconnected");
	else if (event->type == KN_LIFECYCLE_SESSION_LEFT)
	{
		g_kn_mame_status_overlay.clear();
		g_kn_mame_chat_overlay.clear();
		g_kn_mame_chat_input_overlay.clear();
		g_kn_mame_chat_active = false;
		g_kn_mame_chat_pending_chars.clear();
		adapter->chat_messages.clear();
	}
}

static void poll_chat_inbox(kaileron_adapter::impl &adapter)
{
	if (adapter.chat_inbox_path.empty())
		return;

	auto const now = std::chrono::steady_clock::now();
	if (adapter.last_chat_poll_at.time_since_epoch().count() != 0 &&
			now - adapter.last_chat_poll_at < std::chrono::milliseconds(500))
		return;
	adapter.last_chat_poll_at = now;

	std::ifstream file(adapter.chat_inbox_path);
	if (!file)
		return;

	std::string line;
	while (std::getline(file, line))
	{
		std::istringstream stream(line);
		std::string id_text;
		std::string author;
		std::string body;
		if (!std::getline(stream, id_text, '\t') ||
				!std::getline(stream, author, '\t') ||
				!std::getline(stream, body))
			continue;

		char *end = nullptr;
		unsigned long long const id = std::strtoull(id_text.c_str(), &end, 10);
		if ((end && *end) || id <= adapter.last_chat_id)
			continue;

		adapter.last_chat_id = id;
		std::string message = "<" + author + "> " + body;
		append_chat_overlay(adapter, message);
	}
}

static void show_chat_input(kaileron_adapter::impl &adapter)
{
	g_kn_mame_chat_input_overlay = "Chat: " + adapter.chat_text;
}

static void process_chat_input(kaileron_adapter::impl &adapter)
{
	if (adapter.chat_outbox_path.empty())
		return;

	bool const chat_hotkey_down = adapter.machine.ioport().type_pressed(IPT_KAILERON_CHAT);
	bool const chat_hotkey_pressed = chat_hotkey_down && !adapter.chat_hotkey_down;
	adapter.chat_hotkey_down = chat_hotkey_down;

	if (!adapter.chat_active && adapter.machine.ui().is_menu_active())
		return;

	if (!adapter.chat_active && chat_hotkey_pressed)
	{
		adapter.chat_active = true;
		adapter.chat_text.clear();
		adapter.chat_enter_ready = false;
		g_kn_mame_chat_pending_chars.clear();
		g_kn_mame_chat_active = true;
		ui_event event;
		while (adapter.machine.ui_input().pop_event(&event))
		{
		}
		show_chat_input(adapter);
		return;
	}

	if (!adapter.chat_active)
		return;

	if (!chat_hotkey_down)
		adapter.chat_enter_ready = true;

	auto close_chat = [&adapter] ()
	{
		adapter.chat_active = false;
		adapter.chat_text.clear();
		g_kn_mame_chat_active = false;
		g_kn_mame_chat_pending_chars.clear();
		g_kn_mame_chat_input_overlay.clear();
	};
	auto send_chat = [&adapter, &close_chat] ()
	{
		std::string body = normalized_chat_line(adapter.chat_text);
		close_chat();
		if (!body.empty())
			write_chat_outbox(adapter.chat_outbox_path, body);
	};
	auto apply_chat_backspace = [&adapter] ()
	{
		if (adapter.chat_text.empty())
			return false;
		pop_utf8_codepoint(adapter.chat_text);
		return true;
	};
	auto apply_chat_char = [&adapter, &apply_chat_backspace] (char32_t ch)
	{
		if (ch == '\b' || ch == 0x7f)
			return apply_chat_backspace();
		if (ch >= 0x20 && adapter.chat_text.size() < 160)
		{
			append_utf8(adapter.chat_text, ch);
			return true;
		}
		return false;
	};

	bool changed = false;
	bool saw_ime_chat_input = false;
	if (!g_kn_mame_chat_pending_chars.empty())
	{
		for (char32_t ch : g_kn_mame_chat_pending_chars)
		{
			saw_ime_chat_input = true;
			changed = apply_chat_char(ch) || changed;
		}
		g_kn_mame_chat_pending_chars.clear();
	}

	ui_event event;
	while (adapter.machine.ui_input().pop_event(&event))
	{
		if (event.event_type != ui_event::type::IME_CHAR)
			continue;

		if (event.ch == 0x1b)
		{
			close_chat();
			return;
		}
		if (event.ch == '\r' || event.ch == '\n')
		{
			if (!adapter.chat_enter_ready)
				continue;
			send_chat();
			return;
		}
		saw_ime_chat_input = true;
		changed = apply_chat_char(event.ch) || changed;
	}

	if (adapter.chat_enter_ready &&
			(key_pressed_once(adapter.machine, ITEM_ID_ENTER) ||
			key_pressed_once(adapter.machine, ITEM_ID_ENTER_PAD)))
	{
		send_chat();
		return;
	}
	if (!saw_ime_chat_input && key_pressed_once(adapter.machine, ITEM_ID_BACKSPACE))
		changed = apply_chat_backspace() || changed;
	if (changed)
	{
		show_chat_input(adapter);
		return;
	}
}

static void update_playback_status_overlay(kaileron_adapter::impl &adapter)
{
	if (!adapter.show_playback_status)
		return;
	if (adapter.chat_active)
		return;

	auto const now = std::chrono::steady_clock::now();
	if (adapter.last_status_at.time_since_epoch().count() != 0 &&
			now - adapter.last_status_at < std::chrono::milliseconds(750))
		return;

	std::string message;
	if (adapter.spectator && adapter.kn_host_session_get_playback_status)
	{
		KnPlaybackStatus status = {};
		if (adapter.kn_playback_status_init)
			adapter.kn_playback_status_init(&status);
		if (adapter.kn_host_session_get_playback_status(adapter.session, &status) != KN_OK)
			return;

		std::string const short_text = status.short_text;
		std::string const detail_text = status.detail_text;
		message = detail_text.empty()
				? "Kaileron: " + short_text
				: "Kaileron: " + short_text + "\n" + detail_text;
	}
	else
	{
		message = "Kaileron netplay: player " + std::to_string(adapter.player_id + 1) + "/" + std::to_string(adapter.player_count);
		if (adapter.kn_host_session_get_metrics)
		{
			KnMetrics metrics = {};
			if (adapter.kn_host_session_get_metrics(adapter.session, &metrics) == KN_OK)
			{
				u32 const confirm_lag = metrics.current_frame > metrics.confirmed_frame_count
						? metrics.current_frame - metrics.confirmed_frame_count
						: 0;
				message += " confirm_lag=" + std::to_string(confirm_lag);
				if (adapter.show_rollback_stats)
				{
					message += " rollbacks=" + std::to_string(metrics.rollback_count);
					message += " max=" + std::to_string(metrics.max_rollback_frames);
				}
			}
		}
	}

	if (message != adapter.last_status_text ||
			now - adapter.last_status_at >= std::chrono::milliseconds(2000))
	{
		adapter.machine.popmessage("%s", message.c_str());
		g_kn_mame_status_overlay = message;
		adapter.last_status_text = message;
	}
	adapter.last_status_at = now;
}

std::unique_ptr<kaileron_adapter> kaileron_adapter::create(running_machine &machine)
{
	if (!env_enabled("KN_MAME"))
	{
		show_kaileron_status(machine, "Kaileron: not launched by adapter");
		return nullptr;
	}

	show_kaileron_status(machine, "Kaileron: KN_MAME detected");

	auto adapter = std::make_unique<kaileron_adapter>(machine);
	if (!adapter->initialize())
	{
		machine.schedule_exit();
		return nullptr;
	}
	return adapter;
}

kaileron_adapter::kaileron_adapter(running_machine &machine) :
	m_impl(std::make_unique<impl>(machine))
{
}

kaileron_adapter::~kaileron_adapter()
{
	on_exit();
	if (m_impl->session && m_impl->kn_host_session_destroy)
		m_impl->kn_host_session_destroy(m_impl->session);
	m_impl->session = nullptr;

	if (m_impl->library)
		close_sdk_library(m_impl->library);
	m_impl->library = nullptr;
}

bool kaileron_adapter::initialize()
{
	const char *library_path = env_value("KN_SDK_LIB", default_sdk_library_path());
	show_kaileron_status(m_impl->machine, std::string("Kaileron: loading SDK ") + library_path);
	std::string load_error;
	m_impl->library = open_sdk_library(library_path, load_error);
	if (!m_impl->library)
	{
		osd_printf_error("Kaileron: failed to load %s: %s\n", library_path, load_error.c_str());
		show_kaileron_status(m_impl->machine, std::string("Kaileron: SDK load failed ") + library_path);
		return false;
	}
	show_kaileron_status(m_impl->machine, "Kaileron: SDK loaded");

	if (!load_symbol(m_impl->library, "kn_config_init", m_impl->kn_config_init) ||
		!load_symbol(m_impl->library, "kn_callbacks_init", m_impl->kn_callbacks_init) ||
		!load_symbol(m_impl->library, "kn_playback_status_init", m_impl->kn_playback_status_init) ||
		!load_symbol(m_impl->library, "kn_host_session_create", m_impl->kn_host_session_create) ||
		!load_symbol(m_impl->library, "kn_host_session_destroy", m_impl->kn_host_session_destroy) ||
		!load_symbol(m_impl->library, "kn_host_session_tick", m_impl->kn_host_session_tick) ||
		!load_symbol(m_impl->library, "kn_host_session_get_metrics", m_impl->kn_host_session_get_metrics) ||
		!load_symbol(m_impl->library, "kn_host_session_get_playback_status", m_impl->kn_host_session_get_playback_status) ||
		!load_symbol(m_impl->library, "kn_host_session_leave", m_impl->kn_host_session_leave))
	{
		show_kaileron_status(m_impl->machine, "Kaileron: SDK symbol load failed");
		return false;
	}
	show_kaileron_status(m_impl->machine, "Kaileron: SDK symbols loaded");

	const char *server = env_value("KN_SERVER", "");
	m_impl->networked = server[0] != 0;
	const char *session = env_value("KN_SESSION", "mame-bublbobl");
	const char *participant_id = env_value("KN_PARTICIPANT_ID", "");
	const char *participant_token = env_value("KN_PARTICIPANT_TOKEN", "");
	u32 default_players = server[0] ? 2 : 1;
	u32 player_id = env_u32("KN_PLAYER_ID", 0);
	u32 player_count = env_u32("KN_PLAYERS", default_players);
	u64 spectator_id = env_u64("KN_SPECTATOR_ID", 0);
	const char *lobby_url = env_value("KN_LOBBY_URL", "");

	m_impl->player_id = player_id;
	m_impl->player_count = player_count;
	m_impl->spectator_id = spectator_id;
	m_impl->remote_bootstrap_enabled =
			server[0] != 0 && lobby_url[0] != 0;
	m_impl->trace = env_enabled("KN_MAME_TRACE");
	m_impl->frame_runner.set_trace(m_impl->trace);
	m_impl->state_store.set_trace(m_impl->trace);
	m_impl->spectator = env_enabled("KN_SPECTATOR");
	bool const replay_playback = env_enabled("KN_REPLAY_PLAYBACK");
	u32 const requested_runahead = std::min<u32>(env_u32("KN_MAME_RUNAHEAD", 0), 8);
	m_impl->requested_runahead_frames = requested_runahead;
	m_impl->runahead_frames = (!m_impl->spectator && !replay_playback)
			? requested_runahead
			: 0;
	m_impl->verify_presentation_restore = env_enabled("KN_MAME_RUNAHEAD_VERIFY");
	m_impl->show_playback_status = env_enabled("KN_MAME_STATUS");
	m_impl->show_rollback_stats = env_enabled("KN_MAME_SHOW_ROLLBACK_STATS");
	m_impl->chat_inbox_path = env_value("KN_CHAT_INBOX", "");
	m_impl->chat_outbox_path = env_value("KN_CHAT_OUTBOX", "");
	m_impl->determinism_dir = env_value("KN_MAME_DETERMINISM_DIR", "");
	m_impl->determinism_role = m_impl->spectator
			? "s" + std::to_string(spectator_id)
			: "p" + std::to_string(player_id);
	m_impl->local_socd_mode = parse_socd_mode(env_value("KN_SOCD_MODE", "up_priority"));
	if (!m_impl->determinism_dir.empty())
	{
		osd_printf_info(
				"Kaileron determinism: enabled role=%s directory=%s\n",
				m_impl->determinism_role.c_str(),
				m_impl->determinism_dir.c_str());
	}
	if (m_impl->trace)
	{
		osd_printf_info("Kaileron trace: SOCD mode=%s\n", socd_mode_name(m_impl->local_socd_mode));
		osd_printf_info(
				"Kaileron trace: runahead requested=%u enabled=%u verify=%u spectator=%u replay=%u\n",
				requested_runahead,
				m_impl->runahead_frames,
				m_impl->verify_presentation_restore ? 1 : 0,
				m_impl->spectator ? 1 : 0,
				replay_playback ? 1 : 0);
	}
	m_impl->original_throttled = m_impl->machine.video().throttled();
	u32 input_size = resolve_input_size(*m_impl);
	m_impl->input_size = input_size;
	u32 const override_frame_ms = env_u32("KN_MAME_FRAME_MS", 0);
	m_impl->frame_duration_us = override_frame_ms > 0
			? override_frame_ms * 1000
			: frame_duration_us(m_impl->machine);
	if (char const *fixed_time = std::getenv("KN_MAME_FIXED_TIME");
			fixed_time && fixed_time[0])
	{
		char *end = nullptr;
		long long const parsed = std::strtoll(fixed_time, &end, 10);
		if (end && end != fixed_time && !*end)
		{
			m_impl->fixed_rtc_enabled = true;
			m_impl->fixed_rtc_base_time = time_t(parsed);
		}
	}

	KnCallbacks callbacks = {};
	if (m_impl->kn_callbacks_init(&callbacks, m_impl.get()) != KN_OK)
	{
		osd_printf_error("Kaileron: kn_callbacks_init failed\n");
		show_kaileron_status(m_impl->machine, "Kaileron: callbacks init failed");
		return false;
	}
	callbacks.poll_local_input = kn_mame_poll_local_input;
	callbacks.save_state = kn_mame_save_state;
	callbacks.load_state = kn_mame_load_state;
	callbacks.discard_states_before = kn_mame_discard_states_before;
	callbacks.advance_frame = kn_mame_advance_frame;
	if (std::strcmp(env_value("KN_MAME_STATE_HASH", "1"), "0") != 0)
		callbacks.state_hash = kn_mame_state_hash;
	callbacks.set_playback_control = kn_mame_set_playback_control;
	callbacks.on_lifecycle_event = kn_mame_on_lifecycle_event;
	callbacks.serialized_state_size = kn_mame_serialized_state_size;
	callbacks.export_serialized_state = kn_mame_export_serialized_state;
	callbacks.import_serialized_state = kn_mame_import_serialized_state;

	KnConfig config = {};
	if (m_impl->kn_config_init(&config) != KN_OK)
	{
		osd_printf_error("Kaileron: kn_config_init failed\n");
		show_kaileron_status(m_impl->machine, "Kaileron: config init failed");
		return false;
	}
	config.server_addr = server;
	config.session_name = session;
	config.participant_id = participant_id;
	config.participant_token = participant_token;
	config.player_id = player_id;
	config.spectator = m_impl->spectator ? 1 : 0;
	config.spectator_id = spectator_id;
	config.lobby_url = lobby_url;
	config.player_count = player_count;
	config.input_size = input_size_env_is_auto() ? 0 : input_size;
	config.input_delay_frames = env_u32("KN_INPUT_DELAY", 0);
	config.max_rollback_frames = env_u32("KN_MAX_ROLLBACK", 120);
	config.frame_duration_us = m_impl->frame_duration_us;
	config.net_profile.delay_ms = env_u32("KN_DELAY_MS", 0);
	config.net_profile.jitter_ms = env_u32("KN_JITTER_MS", 0);
	config.net_profile.loss_percent = env_u32("KN_LOSS_PERCENT", 0);
	m_impl->state_store.set_capacity(env_u32(
			"KN_MAME_SNAPSHOT_SLOTS",
			config.max_rollback_frames > 0 ? config.max_rollback_frames + 1 : 121));

	show_kaileron_status(m_impl->machine, std::string("Kaileron: creating session ") + session);
	KnResult result = m_impl->kn_host_session_create(&config, &callbacks, &m_impl->session);
	if (result != KN_OK)
	{
		osd_printf_error("Kaileron: kn_host_session_create failed result=%d\n", int(result));
		show_kaileron_status(m_impl->machine, "Kaileron: session create failed result=" + std::to_string(int(result)));
		return false;
	}

	m_impl->initialized = true;
	osd_printf_info(
			"Kaileron: enabled server=%s session=%s player=%u/%u input_size=%u runahead=%u sdk=%s\n",
			server[0] ? server : "(local)",
			session,
			config.player_id,
			config.player_count,
			input_size,
			m_impl->runahead_frames,
			library_path);
	update_playback_status_overlay(*m_impl);
	return true;
}

bool kaileron_adapter::tick()
{
	if (!m_impl->initialized || !m_impl->session)
		return false;
	if (m_impl->machine.scheduled_event_pending())
		return false;
	if (m_impl->remote_bootstrap_enabled &&
			!m_impl->determinism_bootstrap_ready &&
			m_impl->frame_runner.completed_frame_count() < REMOTE_BOOTSTRAP_WARMUP_FRAMES)
		return false;

	switch (prepare_determinism_bootstrap(*m_impl))
	{
	case determinism_bootstrap_result::waiting:
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return true;
	case determinism_bootstrap_result::failed:
		osd_printf_error(
				"Kaileron determinism: bootstrap failed role=%s\n",
				m_impl->determinism_role.c_str());
		m_impl->machine.schedule_exit();
		return true;
	case determinism_bootstrap_result::ready:
		break;
	}

	poll_chat_inbox(*m_impl);
	refresh_chat_overlay(*m_impl);
	process_chat_input(*m_impl);
	m_impl->rollback_replay_active = false;
	m_impl->presentation_ready = false;
	m_impl->sdk_pace_delay_us = 0;
	KnResult result = m_impl->kn_host_session_tick(m_impl->session);
	m_impl->rollback_replay_active = false;
	if (result == KN_ERR_REPLAY_COMPLETE)
	{
		osd_printf_info("Kaileron: replay complete\n");
		show_kaileron_status(m_impl->machine, "Kaileron: replay complete");
		m_impl->machine.schedule_exit();
	}
	else if (result != KN_OK)
	{
		osd_printf_error("Kaileron: tick failed result=%d\n", int(result));
		m_impl->machine.schedule_exit();
	}
	else
	{
		if (!m_impl->determinism_dir.empty() && !m_impl->spectator)
		{
			KnMetrics metrics = {};
			if (m_impl->kn_host_session_get_metrics(m_impl->session, &metrics) != KN_OK)
			{
				osd_printf_error(
						"Kaileron determinism: metrics failed role=%s\n",
						m_impl->determinism_role.c_str());
				m_impl->machine.schedule_exit();
				return true;
			}
			for (size_t index = 0; index < DETERMINISM_CHECKPOINT_FRAMES.size(); index++)
			{
				u32 const checkpoint_frame = DETERMINISM_CHECKPOINT_FRAMES[index];
				if (!m_impl->determinism_checkpoints_written[index] &&
						metrics.confirmed_frame_count > checkpoint_frame)
				{
					if (!write_determinism_confirmed_checkpoint(
							*m_impl, checkpoint_frame))
					{
						osd_printf_error(
								"Kaileron determinism: checkpoint failed role=%s frame=%u\n",
								m_impl->determinism_role.c_str(),
								checkpoint_frame);
						m_impl->machine.schedule_exit();
						return true;
					}
					m_impl->determinism_checkpoints_written[index] = true;
				}
			}
			if (m_impl->determinism_checkpoints_written.back())
			{
				osd_printf_info(
						"Kaileron determinism: completed role=%s frame=%u rollbacks=%u\n",
						m_impl->determinism_role.c_str(),
						metrics.current_frame,
						metrics.rollback_count);
				m_impl->machine.schedule_exit();
				return true;
			}
		}

		if (!run_presentation_runahead(*m_impl))
		{
			osd_printf_error(
					"Kaileron: runahead presentation failed frame=%u\n",
					m_impl->presentation_sdk_frame);
			show_kaileron_status(m_impl->machine, "Kaileron: runahead presentation failed");
			m_impl->machine.schedule_exit();
			return true;
		}
		update_playback_status_overlay(*m_impl);
		if (m_impl->networked && m_impl->sdk_pace_delay_us > 0)
		{
			m_impl->pace_sleep_count++;
			m_impl->pace_sleep_time_us += m_impl->sdk_pace_delay_us;
			std::this_thread::sleep_for(std::chrono::microseconds(m_impl->sdk_pace_delay_us));
		}
	}
	return true;
}

void kaileron_adapter::frame_done(kaileron_frame_mode mode)
{
	m_impl->frame_runner.frame_done(mode);
}

void kaileron_adapter::frame_notifier()
{
	m_impl->frame_runner.frame_notifier();
}

void kaileron_adapter::on_exit()
{
	if (!m_impl->initialized || !m_impl->session || m_impl->reported_exit)
		return;
	g_kn_mame_status_overlay.clear();
	g_kn_mame_chat_overlay.clear();
	g_kn_mame_chat_input_overlay.clear();
	g_kn_mame_chat_active = false;
	g_kn_mame_chat_pending_chars.clear();

	KnPlaybackControl control = {};
	control.target_speed_percent = 100;
	apply_playback_control(*m_impl, control);
	m_impl->kn_host_session_leave(m_impl->session);
	m_impl->reported_exit = true;

	KnMetrics metrics = {};
	if (m_impl->kn_host_session_get_metrics(m_impl->session, &metrics) == KN_OK)
	{
		osd_printf_info(
				"kn_mame_adapter summary frames=%u mame_frames=%u confirmed=%u rollbacks=%u max_rollback=%u stalls=%u pace_sleeps=%u pace_sleep_us=%llu frame_notifiers=%u rollback_replay_frames=%u spectator_catchup_frames=%u speculative_frames=%u runahead_frames=%u presentation_saves=%u presentation_restores=%u saves=%u loads=%u advances=%u injected=%u nonneutral=%u last_input=%02x snapshot_bytes=%u snapshot_slots=%u/%u save_avg_us=%llu load_avg_us=%llu advance_avg_us=%llu discard_us=%llu discarded=%u input_hash=%016llx state_hash=%016llx confirmed_state_frame=%u confirmed_state_hash=%016llx missing=%u nacks=%u\n",
				metrics.current_frame,
				m_impl->frame_runner.completed_frame_count(),
				metrics.confirmed_frame_count,
				metrics.rollback_count,
				metrics.max_rollback_frames,
				metrics.prediction_stall_frames,
				m_impl->pace_sleep_count,
				(unsigned long long)m_impl->pace_sleep_time_us,
				m_impl->frame_runner.frame_notifier_count(),
				m_impl->frame_runner.rollback_resimulation_count(),
				m_impl->frame_runner.spectator_catchup_count(),
				m_impl->frame_runner.speculative_presentation_count(),
				m_impl->runahead_frames,
				m_impl->state_store.presentation_save_count(),
				m_impl->state_store.presentation_restore_count(),
				m_impl->state_store.save_count(),
				m_impl->state_store.load_count(),
				m_impl->frame_runner.advance_count(),
				m_impl->injected_frame_count,
				m_impl->nonneutral_input_count,
				m_impl->last_local_input,
				m_impl->state_store.last_snapshot_size(),
				m_impl->state_store.valid_slot_count(),
				m_impl->state_store.slot_capacity(),
				(unsigned long long)m_impl->state_store.average_save_us(),
				(unsigned long long)m_impl->state_store.average_load_us(),
				(unsigned long long)m_impl->frame_runner.average_advance_us(),
				(unsigned long long)m_impl->state_store.discard_time_us(),
				m_impl->state_store.discard_count(),
				(unsigned long long)metrics.confirmed_input_hash,
				(unsigned long long)metrics.current_state_hash,
				metrics.confirmed_frame_count,
				(unsigned long long)m_impl->state_store.snapshot_hash(metrics.confirmed_frame_count),
				metrics.confirmed_missing_frames,
				metrics.confirmed_nack_sent);
	}
}
