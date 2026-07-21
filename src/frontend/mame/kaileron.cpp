// license:BSD-3-Clause

#include "emu.h"
#include "main.h"
#include "kaileron_adapter.h"

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
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
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

u64 elapsed_us(std::chrono::steady_clock::time_point start)
{
	auto elapsed = std::chrono::steady_clock::now() - start;
	return u64(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
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

static void fnv1a64_append(u64 &hash, u8 const *bytes, std::size_t len)
{
	for (std::size_t index = 0; index < len; index++)
	{
		hash ^= bytes[index];
		hash *= 1099511628211ULL;
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

struct snapshot_slot
{
	u32 frame = 0;
	bool valid = false;
	u64 state_hash = 0;
	std::vector<u8> bytes;
};

} // namespace

struct kaileron_adapter::impl
{
	explicit impl(running_machine &machine) :
		machine(machine)
	{
	}

	running_machine &machine;
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
	std::vector<snapshot_slot> snapshots;
	u32 player_id = 0;
	u32 player_count = 1;
	u32 input_size = 1;
	u32 frame_count = 0;
	u32 save_count = 0;
	u32 load_count = 0;
	u32 advance_count = 0;
	u32 discard_count = 0;
	u32 pace_sleep_count = 0;
	u32 frame_notifier_count = 0;
	u32 rollback_replay_frame_count = 0;
	u32 speculative_frame_count = 0;
	u32 injected_frame_count = 0;
	u32 nonneutral_input_count = 0;
	u32 last_snapshot_size = 0;
	u32 snapshot_capacity = 0;
	u32 snapshot_signature = 0;
	u32 frame_duration_us = 0;
	size_t snapshot_size = 0;
	u64 save_time_us = 0;
	u64 load_time_us = 0;
	u64 advance_time_us = 0;
	u64 discard_time_us = 0;
	u64 pace_sleep_time_us = 0;
	u8 last_local_input = 0;
	u8 last_applied_input[2] = {0xff, 0xff};
	std::chrono::steady_clock::time_point last_status_at = {};
	std::chrono::steady_clock::time_point last_chat_poll_at = {};
	std::vector<mapped_input_field> input_map;
	std::string last_status_text;
	std::string chat_inbox_path;
	std::string chat_outbox_path;
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
	socd_mode local_socd_mode = socd_mode::up_priority;
	bool sdk_playback_override = false;
	bool rollback_replay_active = false;
	bool show_playback_status = false;
	bool show_rollback_stats = false;
};

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

static void apply_mapped_inputs(kaileron_adapter::impl &adapter, const KnInput *players, u32 player_count)
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
		if (input)
			adapter.nonneutral_input_count++;
		if (adapter.trace && player < 2 && input != adapter.last_applied_input[player])
		{
			osd_printf_info("Kaileron trace: apply_input frame=%u player=%u input=%02x machine=%s\n",
					adapter.frame_count, player, input, adapter.machine.system().name);
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
	adapter.injected_frame_count++;
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

static bool ensure_snapshot_size(kaileron_adapter::impl &adapter)
{
	if (adapter.snapshot_size != 0)
		return true;

	size_t const size = ram_state::get_size(adapter.machine.save());
	if (size == 0 || size > std::numeric_limits<u32>::max())
		return false;

	adapter.snapshot_size = size;
	adapter.snapshot_signature = adapter.machine.save().state_signature();
	adapter.last_snapshot_size = u32(size);
	return true;
}

static u32 default_snapshot_capacity(kaileron_adapter::impl const &adapter)
{
	if (adapter.snapshot_capacity > 0)
		return adapter.snapshot_capacity;

	return 121;
}

static void ensure_snapshot_ring(kaileron_adapter::impl &adapter)
{
	if (!adapter.snapshots.empty())
		return;

	u32 const capacity = env_u32("KN_MAME_SNAPSHOT_SLOTS", default_snapshot_capacity(adapter));
	adapter.snapshot_capacity = std::max<u32>(capacity, 1);
	adapter.snapshots.resize(adapter.snapshot_capacity);
}

static bool save_machine_state_to_buffer(kaileron_adapter::impl &adapter, std::vector<u8> &bytes)
{
	if (adapter.machine.scheduled_event_pending())
		return false;
	if (!ensure_snapshot_size(adapter))
		return false;

	bytes.resize(adapter.snapshot_size);
	return adapter.machine.save().write_buffer_with_signature(bytes.data(), bytes.size(), adapter.snapshot_signature) == STATERR_NONE;
}

static bool load_machine_state_from_buffer(kaileron_adapter::impl &adapter, const std::vector<u8> &bytes)
{
	if (bytes.empty())
		return false;
	if (adapter.machine.scheduled_event_pending())
		return false;

	if (!ensure_snapshot_size(adapter))
		return false;

	return adapter.machine.save().read_buffer_with_signature(bytes.data(), bytes.size(), adapter.snapshot_signature) == STATERR_NONE;
}

static snapshot_slot *find_snapshot_slot(kaileron_adapter::impl &adapter, u32 frame)
{
	if (adapter.snapshots.empty())
		return nullptr;

	snapshot_slot &slot = adapter.snapshots[frame % adapter.snapshot_capacity];
	if (!slot.valid || slot.frame != frame)
		return nullptr;
	return &slot;
}

static u32 valid_snapshot_slot_count(kaileron_adapter::impl const &adapter)
{
	u32 count = 0;
	for (snapshot_slot const &slot : adapter.snapshots)
		if (slot.valid)
			count++;
	return count;
}

static bool unstable_state_entry(char const *name)
{
	std::string const entry(name ? name : "");
	// These entries describe host-side scheduling/buffering or Z80 scratch that
	// is not initialized until a matching instruction executes.  They are part
	// of a restorable MAME state, but not a stable cross-process world identity.
	bool const timer_heap_index = entry.rfind("timer/", 0) == 0 &&
			entry.size() >= 8 &&
			entry.compare(entry.size() - 8, 8, "/m_index") == 0;
	bool const asynchronous_sound_buffer = entry.rfind("stream.sound_stream", 0) == 0;
	bool const z80_transient_scratch = entry.size() >= 17 &&
			entry.compare(entry.size() - 17, 17, "/m_shared_data2.w") == 0;
	return timer_heap_index || asynchronous_sound_buffer || z80_transient_scratch;
}

static u64 stable_state_buffer_hash(running_machine &machine, std::vector<u8> const &bytes)
{
	save_manager &save = machine.save();
	size_t payload_size = 0;
	for (int index = 0; index < save.registration_count(); index++)
	{
		void *base = nullptr;
		u32 value_size = 0;
		u32 value_count = 0;
		u32 block_count = 0;
		u32 stride = 0;
		if (!save.indexed_item(index, base, value_size, value_count, block_count, stride))
			return 0;
		payload_size += size_t(value_size) * value_count * block_count;
	}
	if (payload_size > bytes.size())
		return 0;

	u64 hash = 1469598103934665603ULL;
	size_t offset = bytes.size() - payload_size;
	for (int index = 0; index < save.registration_count(); index++)
	{
		void *base = nullptr;
		u32 value_size = 0;
		u32 value_count = 0;
		u32 block_count = 0;
		u32 stride = 0;
		char const *name = save.indexed_item(index, base, value_size, value_count, block_count, stride);
		size_t const size = size_t(value_size) * value_count * block_count;
		if (!name || offset + size > bytes.size())
			return 0;
		if (!unstable_state_entry(name))
		{
			fnv1a64_append(hash, reinterpret_cast<u8 const *>(name), std::strlen(name));
			fnv1a64_append(hash, bytes.data() + offset, size);
		}
		offset += size;
	}
	return hash;
}

static u64 snapshot_hash(kaileron_adapter::impl &adapter, u32 frame)
{
	snapshot_slot *slot = find_snapshot_slot(adapter, frame);
	return slot ? slot->state_hash : 0;
}

static u64 average_us(u64 total, u32 count)
{
	return count > 0 ? total / count : 0;
}

class scoped_kaileron_frame
{
public:
	scoped_kaileron_frame(kaileron_frame_mode mode, bool hide_video) :
		m_previous_mode(g_kn_mame_frame_mode),
		m_previous_hide_video(g_kn_mame_hide_replay_video)
	{
		g_kn_mame_frame_mode = mode;
		g_kn_mame_hide_replay_video = hide_video;
	}

	~scoped_kaileron_frame()
	{
		g_kn_mame_frame_mode = m_previous_mode;
		g_kn_mame_hide_replay_video = m_previous_hide_video;
	}

private:
	kaileron_frame_mode const m_previous_mode;
	bool const m_previous_hide_video;
};

static KnResult advance_mame_frame(
		kaileron_adapter::impl &adapter,
		u32 frame,
		kaileron_frame_mode mode,
		bool hide_video)
{
	scoped_kaileron_frame const frame_scope(mode, hide_video);

	u32 target_frame = adapter.frame_count + 1;
	u32 guard = 0;
	u32 guard_limit = env_u32("KN_MAME_ADVANCE_GUARD", 1000000);
	while (adapter.frame_count < target_frame && !adapter.machine.scheduled_event_pending())
	{
		adapter.machine.scheduler().timeslice();
		if (++guard > guard_limit)
		{
			if (adapter.trace)
				osd_printf_info("Kaileron trace: advance_frame guard exhausted frame=%u current_mame_frames=%u target_mame_frames=%u guard=%u\n",
						frame, adapter.frame_count, target_frame, guard);
			return KN_ERR_CALLBACK;
		}
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

	if (adapter->trace && adapter->save_count < 5)
		osd_printf_info("Kaileron trace: save_state frame=%u\n", frame);

	auto const start = std::chrono::steady_clock::now();
	ensure_snapshot_ring(*adapter);
	snapshot_slot &slot = adapter->snapshots[frame % adapter->snapshot_capacity];
	if (!save_machine_state_to_buffer(*adapter, slot.bytes))
		return KN_ERR_CALLBACK;

	slot.frame = frame;
	slot.state_hash = stable_state_buffer_hash(adapter->machine, slot.bytes);
	slot.valid = true;
	adapter->last_snapshot_size = u32(slot.bytes.size());
	adapter->save_count++;
	adapter->save_time_us += elapsed_us(start);
	if (adapter->trace && adapter->save_count < 6)
		osd_printf_info("Kaileron trace: save_state done frame=%u size=%u\n", frame, adapter->last_snapshot_size);
	return KN_OK;
}

static KnResult KN_CALL kn_mame_load_state(void *user, u32 frame)
{
	auto *adapter = static_cast<kaileron_adapter::impl *>(user);
	if (!adapter)
		return KN_ERR_INVALID_ARGUMENT;
	if (adapter->machine.scheduled_event_pending())
		return KN_OK;

	snapshot_slot *slot = find_snapshot_slot(*adapter, frame);
	if (!slot)
		return KN_ERR_CALLBACK;

	if (adapter->trace)
		osd_printf_info("Kaileron trace: load_state frame=%u size=%u load_count=%u current_mame_frames=%u\n", frame, u32(slot->bytes.size()), adapter->load_count, adapter->frame_count);

	auto const start = std::chrono::steady_clock::now();
	if (!load_machine_state_from_buffer(*adapter, slot->bytes))
		return KN_ERR_CALLBACK;

	adapter->load_count++;
	adapter->load_time_us += elapsed_us(start);
	adapter->rollback_replay_active = true;
	if (adapter->trace)
		osd_printf_info("Kaileron trace: load_state done frame=%u load_count=%u\n", frame, adapter->load_count);
	return KN_OK;
}

static void KN_CALL kn_mame_discard_states_before(void *user, u32 frame)
{
	auto *adapter = static_cast<kaileron_adapter::impl *>(user);
	if (!adapter)
		return;

	auto const start = std::chrono::steady_clock::now();
	u32 discarded = 0;
	for (snapshot_slot &slot : adapter->snapshots)
	{
		if (slot.valid && slot.frame < frame)
		{
			slot.valid = false;
			discarded++;
		}
	}
	if (discarded > 0)
		adapter->discard_count += discarded;
	adapter->discard_time_us += elapsed_us(start);
}

static KnResult KN_CALL kn_mame_advance_frame(void *user, u32 frame, const KnInput *players, u32 player_count)
{
	auto *adapter = static_cast<kaileron_adapter::impl *>(user);
	if (!adapter)
		return KN_ERR_INVALID_ARGUMENT;
	if (adapter->machine.scheduled_event_pending())
		return KN_OK;

	if (adapter->trace && adapter->load_count > 0)
		osd_printf_info("Kaileron trace: advance_frame frame=%u before mame_frames=%u loads=%u\n", frame, adapter->frame_count, adapter->load_count);

	bool const skip_catchup_video = adapter->sdk_playback_override &&
			adapter->sdk_render_interval > 1 &&
			(frame % adapter->sdk_render_interval) != 0;
	kaileron_frame_mode const mode = adapter->rollback_replay_active ?
			kaileron_frame_mode::rollback_replay : kaileron_frame_mode::authoritative;
	bool const hide_video = adapter->rollback_replay_active || skip_catchup_video;

	apply_mapped_inputs(*adapter, players, player_count);

	adapter->in_advance = true;
	auto const start = std::chrono::steady_clock::now();
	KnResult result = advance_mame_frame(*adapter, frame, mode, hide_video);
	if (result != KN_OK)
	{
		adapter->in_advance = false;
		return result;
	}
	adapter->advance_count++;
	adapter->advance_time_us += elapsed_us(start);

	adapter->in_advance = false;
	if (adapter->trace && adapter->load_count > 0)
		osd_printf_info("Kaileron trace: advance_frame done frame=%u after mame_frames=%u advances=%u\n", frame, adapter->frame_count, adapter->advance_count);

	(void)frame;
	return KN_OK;
}

static u64 KN_CALL kn_mame_state_hash(void *user)
{
	auto *adapter = static_cast<kaileron_adapter::impl *>(user);
	if (!adapter)
		return 0;

	if (!ensure_snapshot_size(*adapter))
		return 0;

	std::vector<u8> bytes(adapter->snapshot_size);
	if (adapter->machine.save().write_buffer_with_signature(bytes.data(), bytes.size(), adapter->snapshot_signature) != STATERR_NONE)
		return 0;

	return stable_state_buffer_hash(adapter->machine, bytes);
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
	u32 default_players = server[0] ? 2 : 1;
	u32 player_id = env_u32("KN_PLAYER_ID", 0);
	u32 player_count = env_u32("KN_PLAYERS", default_players);
	u64 spectator_id = env_u64("KN_SPECTATOR_ID", 0);
	const char *lobby_url = env_value("KN_LOBBY_URL", "");

	m_impl->player_id = player_id;
	m_impl->player_count = player_count;
	m_impl->trace = env_enabled("KN_MAME_TRACE");
	m_impl->spectator = env_enabled("KN_SPECTATOR");
	m_impl->show_playback_status = env_enabled("KN_MAME_STATUS");
	m_impl->show_rollback_stats = env_enabled("KN_MAME_SHOW_ROLLBACK_STATS");
	m_impl->chat_inbox_path = env_value("KN_CHAT_INBOX", "");
	m_impl->chat_outbox_path = env_value("KN_CHAT_OUTBOX", "");
	m_impl->local_socd_mode = parse_socd_mode(env_value("KN_SOCD_MODE", "up_priority"));
	if (m_impl->trace)
		osd_printf_info("Kaileron trace: SOCD mode=%s\n", socd_mode_name(m_impl->local_socd_mode));
	m_impl->original_throttled = m_impl->machine.video().throttled();
	u32 input_size = resolve_input_size(*m_impl);
	m_impl->input_size = input_size;
	u32 const override_frame_ms = env_u32("KN_MAME_FRAME_MS", 0);
	m_impl->frame_duration_us = override_frame_ms > 0
			? override_frame_ms * 1000
			: frame_duration_us(m_impl->machine);

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

	KnConfig config = {};
	if (m_impl->kn_config_init(&config) != KN_OK)
	{
		osd_printf_error("Kaileron: kn_config_init failed\n");
		show_kaileron_status(m_impl->machine, "Kaileron: config init failed");
		return false;
	}
	config.server_addr = server;
	config.session_name = session;
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
	m_impl->snapshot_capacity = env_u32(
			"KN_MAME_SNAPSHOT_SLOTS",
			config.max_rollback_frames > 0 ? config.max_rollback_frames + 1 : 121);

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
			"Kaileron: enabled server=%s session=%s player=%u/%u input_size=%u sdk=%s\n",
			server[0] ? server : "(local)",
			session,
			config.player_id,
			config.player_count,
			input_size,
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

	poll_chat_inbox(*m_impl);
	refresh_chat_overlay(*m_impl);
	process_chat_input(*m_impl);
	m_impl->rollback_replay_active = false;
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
	m_impl->frame_count++;
	if (mode == kaileron_frame_mode::rollback_replay)
		m_impl->rollback_replay_frame_count++;
	else if (mode == kaileron_frame_mode::speculative)
		m_impl->speculative_frame_count++;
}

void kaileron_adapter::frame_notifier()
{
	m_impl->frame_notifier_count++;
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
				"kn_mame_adapter summary frames=%u mame_frames=%u confirmed=%u rollbacks=%u max_rollback=%u stalls=%u pace_sleeps=%u pace_sleep_us=%llu frame_notifiers=%u rollback_replay_frames=%u speculative_frames=%u saves=%u loads=%u advances=%u injected=%u nonneutral=%u last_input=%02x snapshot_bytes=%u snapshot_slots=%u/%u save_avg_us=%llu load_avg_us=%llu advance_avg_us=%llu discard_us=%llu discarded=%u input_hash=%016llx state_hash=%016llx confirmed_state_frame=%u confirmed_state_hash=%016llx missing=%u nacks=%u\n",
				metrics.current_frame,
				m_impl->frame_count,
				metrics.confirmed_frame_count,
				metrics.rollback_count,
				metrics.max_rollback_frames,
				metrics.prediction_stall_frames,
				m_impl->pace_sleep_count,
				(unsigned long long)m_impl->pace_sleep_time_us,
				m_impl->frame_notifier_count,
				m_impl->rollback_replay_frame_count,
				m_impl->speculative_frame_count,
				m_impl->save_count,
				m_impl->load_count,
				m_impl->advance_count,
				m_impl->injected_frame_count,
				m_impl->nonneutral_input_count,
				m_impl->last_local_input,
				m_impl->last_snapshot_size,
				valid_snapshot_slot_count(*m_impl),
				u32(m_impl->snapshots.size()),
				(unsigned long long)average_us(m_impl->save_time_us, m_impl->save_count),
				(unsigned long long)average_us(m_impl->load_time_us, m_impl->load_count),
				(unsigned long long)average_us(m_impl->advance_time_us, m_impl->advance_count),
				(unsigned long long)m_impl->discard_time_us,
				m_impl->discard_count,
				(unsigned long long)metrics.confirmed_input_hash,
				(unsigned long long)metrics.current_state_hash,
				metrics.confirmed_frame_count,
				(unsigned long long)snapshot_hash(*m_impl, metrics.confirmed_frame_count),
				metrics.confirmed_missing_frames,
				metrics.confirmed_nack_sent);
	}
}
