// license:BSD-3-Clause

#include "emu.h"
#include "main.h"

#include "kaileron_state_store.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace {

u64 elapsed_us(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - start).count();
}

void fnv1a64_append(u64 &hash, u8 const *bytes, std::size_t len)
{
	for (std::size_t index = 0; index < len; index++)
	{
		hash ^= bytes[index];
		hash *= 1099511628211ULL;
	}
}

bool unstable_state_entry(char const *name)
{
	std::string const entry(name ? name : "");
	// These entries describe host-side scheduling/buffering or Z80 scratch that
	// is not initialized until a matching instruction executes.  They are part
	// of a restorable MAME state, but not a stable cross-process world identity.
	bool const timer_heap_index = entry.rfind("timer/", 0) == 0 &&
			entry.size() >= 8 &&
			entry.compare(entry.size() - 8, 8, "/m_index") == 0;
	bool const lua_scheduler_timer = entry.rfind("timer/lua_engine::resume/", 0) == 0;
	// The output manager mirrors lamps and LEDs for host presentation.  Device
	// postload handlers can immediately regenerate these values from core state.
	bool const derived_host_output = entry.rfind("output/", 0) == 0;
	bool const asynchronous_sound_buffer = entry.rfind("stream.sound_stream", 0) == 0;
	bool const z80_transient_scratch = entry.size() >= 17 &&
			entry.compare(entry.size() - 17, 17, "/m_shared_data2.w") == 0;
	return timer_heap_index ||
			lua_scheduler_timer ||
			derived_host_output ||
			asynchronous_sound_buffer ||
			z80_transient_scratch;
}

bool write_binary_file(std::string const &path, std::vector<u8> const &bytes)
{
	std::filesystem::path const target(path);
	std::filesystem::path temporary(target);
	temporary += ".tmp";
	std::error_code error;
	if (!target.parent_path().empty())
	{
		std::filesystem::create_directories(target.parent_path(), error);
		if (error)
			return false;
	}

	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output)
			return false;
		output.write(reinterpret_cast<char const *>(bytes.data()), std::streamsize(bytes.size()));
		if (!output)
			return false;
	}

	std::filesystem::remove(target, error);
	error.clear();
	std::filesystem::rename(temporary, target, error);
	if (error)
	{
		std::filesystem::remove(temporary, error);
		return false;
	}
	return true;
}

class scoped_presentation_state_io
{
public:
	scoped_presentation_state_io() :
		m_previous(g_kn_mame_presentation_state_io)
	{
		g_kn_mame_presentation_state_io = true;
	}

	~scoped_presentation_state_io()
	{
		g_kn_mame_presentation_state_io = m_previous;
	}

private:
	bool const m_previous;
};

} // anonymous namespace

kaileron_state_store::kaileron_state_store(running_machine &machine) :
	m_machine(machine)
{
}

void kaileron_state_store::set_capacity(u32 capacity) noexcept
{
	if (m_snapshots.empty())
		m_snapshot_capacity = std::max<u32>(capacity, 1);
}

bool kaileron_state_store::ensure_snapshot_size()
{
	size_t const size = m_machine.save().transient_state_size();
	if (size == 0 || size > std::numeric_limits<u32>::max())
		return false;

	u32 const signature = m_machine.save().transient_state_signature();
	if (m_snapshot_size == size && m_snapshot_signature == signature)
		return true;

	// Save registrations can still change while the machine is starting.  Never
	// retain buffers sized from an earlier registration set.
	if (m_snapshot_size != 0)
	{
		m_snapshots.clear();
		m_presentation_snapshot.clear();
		m_presentation_snapshot_hash = 0;
		m_current_snapshot_valid = false;
	}
	m_snapshot_size = size;
	m_snapshot_signature = signature;
	m_last_snapshot_size = u32(size);
	return true;
}

void kaileron_state_store::ensure_snapshot_ring()
{
	if (!m_snapshots.empty())
		return;

	if (m_snapshot_capacity == 0)
		m_snapshot_capacity = 121;
	m_snapshots.resize(m_snapshot_capacity);
}

bool kaileron_state_store::save_machine_state(std::vector<u8> &bytes)
{
	if (m_machine.scheduled_event_pending() || !ensure_snapshot_size())
		return false;

	bytes.resize(m_snapshot_size);
	return m_machine.save().write_transient_buffer(
			bytes.data(), bytes.size(), m_snapshot_signature) == STATERR_NONE;
}

bool kaileron_state_store::load_machine_state(std::vector<u8> const &bytes)
{
	if (bytes.empty() || m_machine.scheduled_event_pending() || !ensure_snapshot_size())
		return false;

	invalidate_current_snapshot();
	return m_machine.save().read_transient_buffer(
			bytes.data(), bytes.size(), m_snapshot_signature) == STATERR_NONE;
}

kaileron_state_store::snapshot_slot *kaileron_state_store::find_slot(u32 frame) noexcept
{
	if (m_snapshots.empty())
		return nullptr;

	snapshot_slot &slot = m_snapshots[frame % m_snapshot_capacity];
	return slot.valid && slot.frame == frame ? &slot : nullptr;
}

kaileron_state_store::snapshot_slot const *kaileron_state_store::find_slot(u32 frame) const noexcept
{
	if (m_snapshots.empty())
		return nullptr;

	snapshot_slot const &slot = m_snapshots[frame % m_snapshot_capacity];
	return slot.valid && slot.frame == frame ? &slot : nullptr;
}

u64 kaileron_state_store::stable_state_hash(std::vector<u8> const &bytes) const
{
	save_manager &save = m_machine.save();
	size_t payload_size = 0;
	for (int index = 0; index < save.registration_count(); index++)
	{
		if (save.indexed_item_is_extended(index))
			continue;
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
		if (save.indexed_item_is_extended(index))
			continue;
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

bool kaileron_state_store::save(u32 frame)
{
	if (m_trace && m_save_count < 5)
		osd_printf_info("Kaileron trace: save_state frame=%u\n", frame);

	auto const start = std::chrono::steady_clock::now();
	if (!ensure_snapshot_size())
		return false;
	ensure_snapshot_ring();
	snapshot_slot &slot = m_snapshots[frame % m_snapshot_capacity];
	slot.bytes.resize(m_snapshot_size);
	if (m_machine.save().write_transient_buffer(
			slot.bytes.data(),
			slot.bytes.size(),
			m_snapshot_signature) != STATERR_NONE)
		return false;

	slot.frame = frame;
	slot.state_hash = stable_state_hash(slot.bytes);
	slot.valid = true;
	m_last_snapshot_size = u32(slot.bytes.size());
	m_save_count++;
	m_save_time_us += elapsed_us(start);
	if (m_trace && m_save_count < 6)
		osd_printf_info("Kaileron trace: save_state done frame=%u size=%u\n", frame, m_last_snapshot_size);
	return true;
}

bool kaileron_state_store::load(u32 frame, u32 completed_frame_count)
{
	snapshot_slot *slot = find_slot(frame);
	if (!slot)
		return false;

	if (m_trace)
		osd_printf_info(
				"Kaileron trace: load_state frame=%u size=%u load_count=%u current_mame_frames=%u\n",
				frame,
				u32(slot->bytes.size()),
				m_load_count,
				completed_frame_count);

	auto const start = std::chrono::steady_clock::now();
	if (!load_machine_state(slot->bytes))
		return false;

	m_load_count++;
	m_load_time_us += elapsed_us(start);
	if (m_trace)
		osd_printf_info("Kaileron trace: load_state done frame=%u load_count=%u\n", frame, m_load_count);
	return true;
}

void kaileron_state_store::discard_before(u32 frame)
{
	auto const start = std::chrono::steady_clock::now();
	u32 discarded = 0;
	for (snapshot_slot &slot : m_snapshots)
	{
		if (slot.valid && slot.frame < frame)
		{
			slot.valid = false;
			discarded++;
		}
	}
	m_discard_count += discarded;
	m_discard_time_us += elapsed_us(start);
}

bool kaileron_state_store::save_presentation()
{
	auto const start = std::chrono::steady_clock::now();
	scoped_presentation_state_io const state_io_scope;
	if (!ensure_snapshot_size())
		return false;

	if (m_current_snapshot_valid &&
			m_presentation_snapshot.size() == m_snapshot_size &&
			m_presentation_snapshot_hash != 0)
	{
		m_presentation_reuse_count++;
	}
	else
	{
		if (!save_machine_state(m_presentation_snapshot))
			return false;
		m_presentation_snapshot_hash = stable_state_hash(m_presentation_snapshot);
		m_presentation_fallback_save_count++;
	}

	// The snapshot is now the presentation restore point.  It no longer proves
	// that the live machine still has the same state once speculation begins.
	m_current_snapshot_valid = false;
	m_presentation_save_count++;
	m_presentation_save_time_us += elapsed_us(start);
	return true;
}

bool kaileron_state_store::restore_presentation()
{
	if (m_presentation_snapshot.empty() || !ensure_snapshot_size())
		return false;

	auto const start = std::chrono::steady_clock::now();
	invalidate_current_snapshot();
	scoped_presentation_state_io const state_io_scope;
	// Presentation may process a user-requested exit while drawing its visible
	// frame.  Restore the authoritative machine state even when that external
	// request is pending; the request itself intentionally remains pending.
	if (m_machine.save().read_transient_buffer(
			m_presentation_snapshot.data(),
			m_presentation_snapshot.size(),
			m_snapshot_signature) != STATERR_NONE)
		return false;

	m_presentation_restore_count++;
	m_presentation_restore_time_us += elapsed_us(start);
	return true;
}

bool kaileron_state_store::verify_presentation_restore()
{
	if (m_presentation_snapshot.empty() || !ensure_snapshot_size())
		return false;

	save_manager &save = m_machine.save();
	size_t payload_size = 0;
	for (int index = 0; index < save.registration_count(); index++)
	{
		if (save.indexed_item_is_extended(index))
			continue;
		void *base = nullptr;
		u32 value_size = 0;
		u32 value_count = 0;
		u32 block_count = 0;
		u32 stride = 0;
		if (!save.indexed_item(index, base, value_size, value_count, block_count, stride))
			return false;
		payload_size += size_t(value_size) * value_count * block_count;
	}
	if (payload_size > m_presentation_snapshot.size())
		return false;

	u32 differences = 0;
	size_t offset = m_presentation_snapshot.size() - payload_size;
	for (int index = 0; index < save.registration_count(); index++)
	{
		if (save.indexed_item_is_extended(index))
			continue;
		void *base = nullptr;
		u32 value_size = 0;
		u32 value_count = 0;
		u32 block_count = 0;
		u32 stride = 0;
		char const *name = save.indexed_item(index, base, value_size, value_count, block_count, stride);
		size_t const block_size = size_t(value_size) * value_count;
		size_t const size = block_size * block_count;
		if (!name || !base || offset + size > m_presentation_snapshot.size())
			return false;
		bool changed = false;
		u8 const *live = static_cast<u8 const *>(base);
		for (u32 block = 0; block < block_count; block++)
		{
			if (std::memcmp(
					m_presentation_snapshot.data() + offset,
					live,
					block_size) != 0)
				changed = true;
			offset += block_size;
			live += stride;
		}
		if (!unstable_state_entry(name) && changed)
		{
			if (differences < 16)
				osd_printf_error(
						"Kaileron: runahead restore changed state entry=%s size=%u\n",
						name,
						u32(size));
			differences++;
		}
	}
	if (differences)
		osd_printf_error(
				"Kaileron: runahead restore changed stable_entries=%u\n",
				differences);
	return differences == 0;
}

bool kaileron_state_store::export_current(std::vector<u8> &bytes, u64 &state_hash)
{
	if (!save_machine_state(bytes))
		return false;

	u64 const transient_hash = stable_state_hash(bytes);
	if (transient_hash == 0)
		return false;

	std::vector<u8> extension;
	if (!m_machine.save().write_extended_state(extension))
		return false;
	state_hash = transient_hash;
	if (!extension.empty())
		fnv1a64_append(state_hash, extension.data(), extension.size());
	bytes.insert(bytes.end(), extension.begin(), extension.end());
	return true;
}

bool kaileron_state_store::import_current(u8 const *bytes, size_t size, u64 &state_hash)
{
	if (!bytes || m_machine.scheduled_event_pending() || !ensure_snapshot_size())
		return false;
	if (size < m_snapshot_size)
		return false;

	invalidate_current_snapshot();
	if (m_machine.save().read_transient_buffer(
			bytes, m_snapshot_size, m_snapshot_signature) != STATERR_NONE)
		return false;
	if (!m_machine.save().read_extended_state(
			bytes + m_snapshot_size,
			size - m_snapshot_size))
		return false;

	for (snapshot_slot &slot : m_snapshots)
		slot.valid = false;
	m_presentation_snapshot.clear();
	m_presentation_snapshot_hash = 0;
	m_current_snapshot_valid = false;
	state_hash = current_state_hash();
	return state_hash != 0;
}

bool kaileron_state_store::export_current(std::string const &path, u64 &state_hash)
{
	std::vector<u8> bytes;
	return export_current(bytes, state_hash) && write_binary_file(path, bytes);
}

bool kaileron_state_store::import_current(std::string const &path, u64 &state_hash)
{
	if (m_machine.scheduled_event_pending() || !ensure_snapshot_size())
		return false;

	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input)
		return false;
	std::streamoff const file_size = input.tellg();
	if (file_size < std::streamoff(m_snapshot_size) ||
			file_size > std::streamoff(std::numeric_limits<u32>::max()))
		return false;
	input.seekg(0);
	std::vector<u8> bytes(static_cast<size_t>(file_size), u8(0));
	input.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size()));
	return input && import_current(bytes.data(), bytes.size(), state_hash);
}

bool kaileron_state_store::write_current_manifest(std::string const &path, u32 frame)
{
	if (m_machine.scheduled_event_pending() || !ensure_snapshot_size())
		return false;

	std::vector<u8> bytes(m_snapshot_size);
	if (m_machine.save().write_transient_buffer(
			bytes.data(), bytes.size(), m_snapshot_signature) != STATERR_NONE)
		return false;
	return write_manifest(path, frame, bytes);
}

bool kaileron_state_store::export_snapshot(
		u32 frame,
		std::vector<u8> &bytes,
		u64 &state_hash) const
{
	snapshot_slot const *slot = find_slot(frame);
	if (!slot || slot->state_hash == 0)
		return false;
	bytes = slot->bytes;
	state_hash = slot->state_hash;
	return true;
}

bool kaileron_state_store::export_snapshot(
		u32 frame,
		std::string const &path,
		u64 &state_hash)
{
	snapshot_slot const *slot = find_slot(frame);
	if (!slot)
		return false;
	state_hash = slot->state_hash;
	return state_hash != 0 && write_binary_file(path, slot->bytes);
}

bool kaileron_state_store::write_snapshot_manifest(u32 frame, std::string const &path)
{
	snapshot_slot const *slot = find_slot(frame);
	return slot && write_manifest(path, frame, slot->bytes);
}

bool kaileron_state_store::write_manifest(
		std::string const &path,
		u32 frame,
		std::vector<u8> const &bytes)
{
	save_manager &save = m_machine.save();
	size_t payload_size = 0;
	for (int index = 0; index < save.registration_count(); index++)
	{
		if (save.indexed_item_is_extended(index))
			continue;
		void *base = nullptr;
		u32 value_size = 0;
		u32 value_count = 0;
		u32 block_count = 0;
		u32 stride = 0;
		if (!save.indexed_item(index, base, value_size, value_count, block_count, stride))
			return false;
		payload_size += size_t(value_size) * value_count * block_count;
	}
	if (payload_size > bytes.size())
		return false;

	std::filesystem::path const target(path);
	std::filesystem::path temporary(target);
	temporary += ".tmp";
	std::error_code error;
	if (!target.parent_path().empty())
	{
		std::filesystem::create_directories(target.parent_path(), error);
		if (error)
			return false;
	}

	std::ofstream output(temporary, std::ios::trunc);
	if (!output)
		return false;
	output << "# frame\t" << frame << "\n";
	output << "# aggregate\t" << std::hex << std::setw(16) << std::setfill('0')
		   << stable_state_hash(bytes) << std::dec << "\n";
	output << "name\tstable\tsize\thash\n";

	size_t offset = bytes.size() - payload_size;
	for (int index = 0; index < save.registration_count(); index++)
	{
		if (save.indexed_item_is_extended(index))
			continue;
		void *base = nullptr;
		u32 value_size = 0;
		u32 value_count = 0;
		u32 block_count = 0;
		u32 stride = 0;
		char const *raw_name = save.indexed_item(
				index, base, value_size, value_count, block_count, stride);
		size_t const size = size_t(value_size) * value_count * block_count;
		if (!raw_name || offset + size > bytes.size())
			return false;

		std::string name(raw_name);
		std::replace_if(name.begin(), name.end(), [] (char ch) {
			return ch == '\t' || ch == '\r' || ch == '\n';
		}, ' ');
		u64 hash = 1469598103934665603ULL;
		fnv1a64_append(hash, reinterpret_cast<u8 const *>(raw_name), std::strlen(raw_name));
		fnv1a64_append(hash, bytes.data() + offset, size);
		output << name << '\t'
			   << (unstable_state_entry(raw_name) ? 0 : 1) << '\t'
			   << size << '\t'
			   << std::hex << std::setw(16) << std::setfill('0') << hash << std::dec << '\n';
		offset += size;
	}
	if (!output)
		return false;
	output.close();
	if (!output)
		return false;

	std::filesystem::remove(target, error);
	error.clear();
	std::filesystem::rename(temporary, target, error);
	if (error)
	{
		std::filesystem::remove(temporary, error);
		return false;
	}
	return true;
}

u64 kaileron_state_store::current_state_hash()
{
	// The SDK requests this on the hot per-frame path.  Program overlays are
	// intentionally bootstrap-only and are hashed by export_current(); hashing
	// their multi-megabyte payload here would make ordinary rollback CPU-bound.
	if (!ensure_snapshot_size())
	{
		m_current_snapshot_valid = false;
		return 0;
	}

	m_presentation_snapshot.resize(m_snapshot_size);
	if (m_machine.save().write_transient_buffer(
			m_presentation_snapshot.data(),
			m_presentation_snapshot.size(),
			m_snapshot_signature) != STATERR_NONE)
	{
		m_current_snapshot_valid = false;
		return 0;
	}

	m_presentation_snapshot_hash = stable_state_hash(m_presentation_snapshot);
	m_current_snapshot_valid = m_presentation_snapshot_hash != 0;
	return m_presentation_snapshot_hash;
}

u64 kaileron_state_store::snapshot_hash(u32 frame) const noexcept
{
	snapshot_slot const *slot = find_slot(frame);
	return slot ? slot->state_hash : 0;
}

u32 kaileron_state_store::valid_slot_count() const noexcept
{
	u32 count = 0;
	for (snapshot_slot const &slot : m_snapshots)
		if (slot.valid)
			count++;
	return count;
}

u64 kaileron_state_store::average_save_us() const noexcept
{
	return m_save_count ? m_save_time_us / m_save_count : 0;
}

u64 kaileron_state_store::average_load_us() const noexcept
{
	return m_load_count ? m_load_time_us / m_load_count : 0;
}

u64 kaileron_state_store::average_presentation_save_us() const noexcept
{
	return m_presentation_save_count ? m_presentation_save_time_us / m_presentation_save_count : 0;
}

u64 kaileron_state_store::average_presentation_restore_us() const noexcept
{
	return m_presentation_restore_count ? m_presentation_restore_time_us / m_presentation_restore_count : 0;
}
