// license:BSD-3-Clause

#include "emu.h"
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
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern bool g_kn_mame_hide_replay_video;
extern bool g_kn_mame_suppress_replay_audio;
std::string g_kn_mame_status_overlay;

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
	std::unordered_map<u32, std::vector<u8>> snapshots;
	u32 player_id = 0;
	u32 player_count = 1;
	u32 input_size = 1;
	u32 frame_count = 0;
	u32 save_count = 0;
	u32 load_count = 0;
	u32 advance_count = 0;
	u32 runahead_frames = 0;
	u32 runahead_count = 0;
	u32 injected_frame_count = 0;
	u32 nonneutral_input_count = 0;
	u32 last_snapshot_size = 0;
	u32 frame_duration_us = 0;
	u8 last_local_input = 0;
	u8 last_applied_input[2] = {0xff, 0xff};
	std::chrono::steady_clock::time_point last_status_at = {};
	std::chrono::steady_clock::time_point last_chat_poll_at = {};
	std::vector<mapped_input_field> input_map;
	std::string last_status_text;
	std::string chat_inbox_path;
	std::string chat_outbox_path;
	std::string chat_text;
	u64 last_chat_id = 0;
	bool warned_missing_ports = false;
	bool input_map_built = false;
	bool trace = false;
	bool initialized = false;
	bool chat_active = false;
	bool reported_exit = false;
	bool in_advance = false;
	bool in_runahead = false;
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
	bool hide_replay_video = false;
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

static bool mapped_live_input_pressed(running_machine &machine, mapped_input_field const &mapped)
{
	return mapped.field &&
			mapped.field->enabled() &&
			!machine.ui().is_menu_active() &&
			((mapped.field->port().live().digital & mapped.field->mask()) != 0);
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

static bool key_pressed_once(running_machine &machine, input_item_id item)
{
	return machine.input().code_pressed_once(machine.input().code_from_itemid(item));
}

static bool key_pressed(running_machine &machine, input_item_id item)
{
	return machine.input().code_pressed(machine.input().code_from_itemid(item));
}

static bool shift_pressed(running_machine &machine)
{
	return key_pressed(machine, ITEM_ID_LSHIFT) || key_pressed(machine, ITEM_ID_RSHIFT);
}

static bool append_raw_chat_key(running_machine &machine, std::string &text)
{
	bool const shift = shift_pressed(machine);

	for (int item = ITEM_ID_A; item <= ITEM_ID_Z; item++)
	{
		if (key_pressed_once(machine, input_item_id(item)))
		{
			char const base = shift ? 'A' : 'a';
			text.push_back(char(base + (item - ITEM_ID_A)));
			return true;
		}
	}

	static const char normal_digits[] = "0123456789";
	static const char shifted_digits[] = ")!@#$%^&*(";
	for (int item = ITEM_ID_0; item <= ITEM_ID_9; item++)
	{
		if (key_pressed_once(machine, input_item_id(item)))
		{
			int const index = item - ITEM_ID_0;
			text.push_back(shift ? shifted_digits[index] : normal_digits[index]);
			return true;
		}
	}

	struct key_char
	{
		input_item_id item;
		char normal;
		char shifted;
	};
	static constexpr key_char keys[] = {
		{ ITEM_ID_SPACE, ' ', ' ' },
		{ ITEM_ID_MINUS, '-', '_' },
		{ ITEM_ID_EQUALS, '=', '+' },
		{ ITEM_ID_OPENBRACE, '[', '{' },
		{ ITEM_ID_CLOSEBRACE, ']', '}' },
		{ ITEM_ID_BACKSLASH, '\\', '|' },
		{ ITEM_ID_COLON, ';', ':' },
		{ ITEM_ID_QUOTE, '\'', '"' },
		{ ITEM_ID_COMMA, ',', '<' },
		{ ITEM_ID_STOP, '.', '>' },
		{ ITEM_ID_SLASH, '/', '?' },
		{ ITEM_ID_TILDE, '`', '~' },
	};
	for (key_char const &key : keys)
	{
		if (key_pressed_once(machine, key.item))
		{
			text.push_back(shift ? key.shifted : key.normal);
			return true;
		}
	}

	return false;
}

static bool mapped_raw_input_pressed(running_machine &machine, mapped_input_field const &mapped)
{
	return mapped.field &&
			mapped.field->enabled() &&
			!machine.ui().is_menu_active() &&
			machine.input().seq_pressed(mapped.field->seq());
}

static bool mapped_slot(mapped_input_field const &mapped, u32 slot)
{
	u32 byte = 0;
	u8 bit = 0;
	input_slot(slot, byte, bit);
	return mapped.byte == byte && mapped.bit == bit;
}

static bool mapped_direction_field(mapped_input_field const &mapped)
{
	return mapped_slot(mapped, KN_SLOT_UP) ||
			mapped_slot(mapped, KN_SLOT_DOWN) ||
			mapped_slot(mapped, KN_SLOT_LEFT) ||
			mapped_slot(mapped, KN_SLOT_RIGHT);
}

static bool mapped_local_input_pressed(running_machine &machine, mapped_input_field const &mapped)
{
	return mapped_direction_field(mapped) ?
			mapped_raw_input_pressed(machine, mapped) :
			mapped_live_input_pressed(machine, mapped);
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

static void apply_mapped_inputs(kaileron_adapter::impl &adapter, const KnInput *players, u32 player_count, bool count_stats)
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
	if (count_stats)
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
		scripted_digital_input(out_input->bytes, out_input->len, input_frame, adapter->player_id);
		adapter->last_local_input = out_input->bytes[0];
	}
	return KN_OK;
}

static bool save_machine_state_to_buffer(kaileron_adapter::impl &adapter, std::vector<u8> &bytes)
{
	if (adapter.machine.scheduled_event_pending())
		return false;

	int size = ram_state::get_size(adapter.machine.save());
	if (size <= 0)
		return false;

	bytes.resize(size);
	return adapter.machine.save().write_buffer(bytes.data(), bytes.size()) == STATERR_NONE;
}

static bool load_machine_state_from_buffer(kaileron_adapter::impl &adapter, const std::vector<u8> &bytes)
{
	if (bytes.empty())
		return false;

	return adapter.machine.save().read_buffer(bytes.data(), bytes.size()) == STATERR_NONE;
}

static KnResult advance_mame_frame(kaileron_adapter::impl &adapter, u32 frame, bool hide_video, bool mute_audio)
{
	bool const previous_hide_replay_video = g_kn_mame_hide_replay_video;
	bool const previous_suppress_replay_audio = g_kn_mame_suppress_replay_audio;
	if (hide_video)
		g_kn_mame_hide_replay_video = true;
	if (mute_audio)
		g_kn_mame_suppress_replay_audio = true;

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
			g_kn_mame_hide_replay_video = previous_hide_replay_video;
			g_kn_mame_suppress_replay_audio = previous_suppress_replay_audio;
			return KN_ERR_CALLBACK;
		}
	}

	g_kn_mame_hide_replay_video = previous_hide_replay_video;
	g_kn_mame_suppress_replay_audio = previous_suppress_replay_audio;
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

	std::vector<u8> bytes;
	if (!save_machine_state_to_buffer(*adapter, bytes))
		return KN_ERR_CALLBACK;

	adapter->last_snapshot_size = u32(bytes.size());
	adapter->snapshots[frame] = std::move(bytes);
	adapter->save_count++;
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

	auto found = adapter->snapshots.find(frame);
	if (found == adapter->snapshots.end())
		return KN_ERR_CALLBACK;

	if (adapter->trace)
		osd_printf_info("Kaileron trace: load_state frame=%u size=%u load_count=%u current_mame_frames=%u\n", frame, u32(found->second.size()), adapter->load_count, adapter->frame_count);

	if (!load_machine_state_from_buffer(*adapter, found->second))
		return KN_ERR_CALLBACK;

	adapter->load_count++;
	adapter->hide_replay_video = true;
	if (adapter->trace)
		osd_printf_info("Kaileron trace: load_state done frame=%u load_count=%u\n", frame, adapter->load_count);
	return KN_OK;
}

static void KN_CALL kn_mame_discard_states_before(void *user, u32 frame)
{
	auto *adapter = static_cast<kaileron_adapter::impl *>(user);
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
	bool const hide_video = adapter->hide_replay_video || skip_catchup_video;
	bool const runahead_enabled = adapter->runahead_frames > 0 && !hide_video && !adapter->spectator;

	apply_mapped_inputs(*adapter, players, player_count, true);

	adapter->in_advance = true;
	KnResult result = advance_mame_frame(*adapter, frame, hide_video || runahead_enabled, hide_video);
	if (result != KN_OK)
	{
		adapter->in_advance = false;
		return result;
	}
	adapter->advance_count++;

	if (runahead_enabled)
	{
		if (adapter->machine.scheduled_event_pending())
		{
			adapter->in_advance = false;
			return KN_OK;
		}

		std::vector<u8> runahead_snapshot;
		if (!save_machine_state_to_buffer(*adapter, runahead_snapshot))
		{
			adapter->in_advance = false;
			return KN_ERR_CALLBACK;
		}

		adapter->in_runahead = true;
		for (u32 lookahead = 0; lookahead < adapter->runahead_frames; lookahead++)
		{
			bool const final_lookahead_frame = lookahead + 1 == adapter->runahead_frames;
			apply_mapped_inputs(*adapter, players, player_count, false);
			result = advance_mame_frame(*adapter, frame, !final_lookahead_frame, true);
			if (result != KN_OK)
			{
				adapter->in_runahead = false;
				adapter->in_advance = false;
				return result;
			}
		}
		adapter->in_runahead = false;

		if (!load_machine_state_from_buffer(*adapter, runahead_snapshot))
		{
			adapter->in_advance = false;
			return KN_ERR_CALLBACK;
		}
		adapter->runahead_count++;
	}
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

	int size = ram_state::get_size(adapter->machine.save());
	if (size <= 0)
		return 0;

	std::vector<u8> bytes(size);
	if (adapter->machine.save().write_buffer(bytes.data(), bytes.size()) != STATERR_NONE)
		return 0;

	return fnv1a64(bytes.data(), bytes.size());
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
		g_kn_mame_status_overlay.clear();
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
		std::string message = author + ": " + body;
		adapter.machine.popmessage("%s", message.c_str());
		g_kn_mame_status_overlay = message;
		adapter.last_status_text = message;
	}
}

static void show_chat_input(kaileron_adapter::impl &adapter)
{
	std::string message = "Chat: " + adapter.chat_text;
	adapter.machine.popmessage("%s", message.c_str());
	g_kn_mame_status_overlay = message;
	adapter.last_status_text = message;
	adapter.last_status_at = std::chrono::steady_clock::now();
}

static void process_chat_input(kaileron_adapter::impl &adapter)
{
	if (adapter.chat_outbox_path.empty())
		return;

	input_code const toggle_code = adapter.machine.input().code_from_itemid(ITEM_ID_F8);
	if (adapter.machine.input().code_pressed_once(toggle_code))
	{
		adapter.chat_active = !adapter.chat_active;
		if (adapter.chat_active)
			adapter.chat_text.clear();
		show_chat_input(adapter);
	}

	if (!adapter.chat_active)
		return;

	if (key_pressed_once(adapter.machine, ITEM_ID_ESC))
	{
		adapter.chat_active = false;
		adapter.chat_text.clear();
		adapter.machine.popmessage("Chat canceled");
		g_kn_mame_status_overlay = "Chat canceled";
		return;
	}
	if (key_pressed_once(adapter.machine, ITEM_ID_ENTER) ||
			key_pressed_once(adapter.machine, ITEM_ID_ENTER_PAD))
	{
		std::string body = normalized_chat_line(adapter.chat_text);
		adapter.chat_active = false;
		adapter.chat_text.clear();
		if (!body.empty())
		{
			write_chat_outbox(adapter.chat_outbox_path, body);
			adapter.machine.popmessage("Chat sent: %s", body.c_str());
			g_kn_mame_status_overlay = "Chat sent: " + body;
		}
		else
		{
			adapter.machine.popmessage("Chat canceled");
			g_kn_mame_status_overlay = "Chat canceled";
		}
		return;
	}
	if (key_pressed_once(adapter.machine, ITEM_ID_BACKSPACE))
	{
		pop_utf8_codepoint(adapter.chat_text);
		show_chat_input(adapter);
		return;
	}
	if (append_raw_chat_key(adapter.machine, adapter.chat_text))
	{
		show_chat_input(adapter);
		return;
	}

	ui_event event;
	bool changed = false;
	while (adapter.machine.ui_input().pop_event(&event))
	{
		if (event.event_type != ui_event::type::IME_CHAR)
			continue;

		if (event.ch == 0x1b)
		{
			adapter.chat_active = false;
			adapter.chat_text.clear();
			adapter.machine.popmessage("Chat canceled");
			g_kn_mame_status_overlay = "Chat canceled";
			return;
		}
		if (event.ch == '\r' || event.ch == '\n')
		{
			std::string body = normalized_chat_line(adapter.chat_text);
			adapter.chat_active = false;
			adapter.chat_text.clear();
			if (!body.empty())
			{
				write_chat_outbox(adapter.chat_outbox_path, body);
				adapter.machine.popmessage("Chat sent: %s", body.c_str());
				g_kn_mame_status_overlay = "Chat sent: " + body;
			}
			else
			{
				adapter.machine.popmessage("Chat canceled");
				g_kn_mame_status_overlay = "Chat canceled";
			}
			return;
		}
		if (event.ch == '\b' || event.ch == 0x7f)
		{
			pop_utf8_codepoint(adapter.chat_text);
			changed = true;
			continue;
		}
		if (event.ch >= 0x20 && adapter.chat_text.size() < 160)
		{
			append_utf8(adapter.chat_text, event.ch);
			changed = true;
		}
	}

	if (changed)
		show_chat_input(adapter);
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
	m_impl->runahead_frames = m_impl->spectator ? 0 : std::min<u32>(env_u32("KN_MAME_RUNAHEAD", 0), 2);
	m_impl->local_socd_mode = parse_socd_mode(env_value("KN_SOCD_MODE", "up_priority"));
	if (m_impl->trace)
	{
		osd_printf_info("Kaileron trace: SOCD mode=%s\n", socd_mode_name(m_impl->local_socd_mode));
		osd_printf_info("Kaileron trace: runahead_frames=%u\n", m_impl->runahead_frames);
	}
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

	poll_chat_inbox(*m_impl);
	process_chat_input(*m_impl);
	m_impl->hide_replay_video = false;
	m_impl->sdk_pace_delay_us = 0;
	KnResult result = m_impl->kn_host_session_tick(m_impl->session);
	m_impl->hide_replay_video = false;
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
			std::this_thread::sleep_for(std::chrono::microseconds(m_impl->sdk_pace_delay_us));
	}
	return true;
}

void kaileron_adapter::frame_done()
{
	m_impl->frame_count++;
}

void kaileron_adapter::on_exit()
{
	if (!m_impl->initialized || !m_impl->session || m_impl->reported_exit)
		return;
	g_kn_mame_status_overlay.clear();

	KnPlaybackControl control = {};
	control.target_speed_percent = 100;
	apply_playback_control(*m_impl, control);
	m_impl->kn_host_session_leave(m_impl->session);
	m_impl->reported_exit = true;

	KnMetrics metrics = {};
	if (m_impl->kn_host_session_get_metrics(m_impl->session, &metrics) == KN_OK)
	{
		osd_printf_info(
				"kn_mame_adapter summary frames=%u mame_frames=%u confirmed=%u rollbacks=%u max_rollback=%u saves=%u loads=%u advances=%u runahead=%u runahead_frames=%u injected=%u nonneutral=%u last_input=%02x snapshot_bytes=%u input_hash=%016llx state_hash=%016llx missing=%u nacks=%u\n",
				metrics.current_frame,
				m_impl->frame_count,
				metrics.confirmed_frame_count,
				metrics.rollback_count,
				metrics.max_rollback_frames,
				m_impl->save_count,
				m_impl->load_count,
				m_impl->advance_count,
				m_impl->runahead_count,
				m_impl->runahead_frames,
				m_impl->injected_frame_count,
				m_impl->nonneutral_input_count,
				m_impl->last_local_input,
				m_impl->last_snapshot_size,
				(unsigned long long)metrics.confirmed_input_hash,
				(unsigned long long)metrics.current_state_hash,
				metrics.confirmed_missing_frames,
				metrics.confirmed_nack_sent);
	}
}
