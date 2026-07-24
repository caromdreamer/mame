// license:BSD-3-Clause

#include "emu.h"
#include "main.h"

#include "kaileron_state_store.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
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
	bool const asynchronous_sound_buffer = entry.rfind("stream.sound_stream", 0) == 0;
	bool const z80_transient_scratch = entry.size() >= 17 &&
			entry.compare(entry.size() - 17, 17, "/m_shared_data2.w") == 0;
	return timer_heap_index || asynchronous_sound_buffer || z80_transient_scratch;
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
	if (m_snapshot_size != 0)
		return true;

	size_t const size = ram_state::get_size(m_machine.save());
	if (size == 0 || size > std::numeric_limits<u32>::max())
		return false;

	m_snapshot_size = size;
	m_snapshot_signature = m_machine.save().state_signature();
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
	return m_machine.save().write_buffer_with_signature(
			bytes.data(), bytes.size(), m_snapshot_signature) == STATERR_NONE;
}

bool kaileron_state_store::load_machine_state(std::vector<u8> const &bytes)
{
	if (bytes.empty() || m_machine.scheduled_event_pending() || !ensure_snapshot_size())
		return false;

	return m_machine.save().read_buffer_with_signature(
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

bool kaileron_state_store::save(u32 frame)
{
	if (m_trace && m_save_count < 5)
		osd_printf_info("Kaileron trace: save_state frame=%u\n", frame);

	auto const start = std::chrono::steady_clock::now();
	ensure_snapshot_ring();
	snapshot_slot &slot = m_snapshots[frame % m_snapshot_capacity];
	if (!save_machine_state(slot.bytes))
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
	scoped_presentation_state_io const state_io_scope;
	if (!save_machine_state(m_presentation_snapshot))
		return false;

	m_presentation_snapshot_hash = stable_state_hash(m_presentation_snapshot);
	m_presentation_save_count++;
	return true;
}

bool kaileron_state_store::restore_presentation()
{
	if (m_presentation_snapshot.empty() || !ensure_snapshot_size())
		return false;

	scoped_presentation_state_io const state_io_scope;
	// Presentation may process a user-requested exit while drawing its visible
	// frame.  Restore the authoritative machine state even when that external
	// request is pending; the request itself intentionally remains pending.
	if (m_machine.save().read_buffer_with_signature(
			m_presentation_snapshot.data(),
			m_presentation_snapshot.size(),
			m_snapshot_signature) != STATERR_NONE)
		return false;

	m_presentation_restore_count++;
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

u64 kaileron_state_store::current_state_hash()
{
	if (!ensure_snapshot_size())
		return 0;

	std::vector<u8> bytes(m_snapshot_size);
	if (m_machine.save().write_buffer_with_signature(
			bytes.data(), bytes.size(), m_snapshot_signature) != STATERR_NONE)
		return 0;
	return stable_state_hash(bytes);
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
