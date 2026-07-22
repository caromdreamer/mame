// license:BSD-3-Clause

#ifndef MAME_FRONTEND_MAME_KAILERON_STATE_STORE_H
#define MAME_FRONTEND_MAME_KAILERON_STATE_STORE_H

#pragma once

#include "emu.h"

#include <vector>

class kaileron_state_store
{
public:
	explicit kaileron_state_store(running_machine &machine);

	void set_trace(bool trace) noexcept { m_trace = trace; }
	void set_capacity(u32 capacity) noexcept;
	bool save(u32 frame);
	bool load(u32 frame, u32 completed_frame_count);
	void discard_before(u32 frame);
	u64 current_state_hash();
	u64 snapshot_hash(u32 frame) const noexcept;

	u32 save_count() const noexcept { return m_save_count; }
	u32 load_count() const noexcept { return m_load_count; }
	u32 discard_count() const noexcept { return m_discard_count; }
	u32 last_snapshot_size() const noexcept { return m_last_snapshot_size; }
	u32 valid_slot_count() const noexcept;
	u32 slot_capacity() const noexcept { return u32(m_snapshots.size()); }
	u64 average_save_us() const noexcept;
	u64 average_load_us() const noexcept;
	u64 discard_time_us() const noexcept { return m_discard_time_us; }

private:
	struct snapshot_slot
	{
		u32 frame = 0;
		bool valid = false;
		u64 state_hash = 0;
		std::vector<u8> bytes;
	};

	bool ensure_snapshot_size();
	void ensure_snapshot_ring();
	bool save_machine_state(std::vector<u8> &bytes);
	bool load_machine_state(std::vector<u8> const &bytes);
	snapshot_slot *find_slot(u32 frame) noexcept;
	snapshot_slot const *find_slot(u32 frame) const noexcept;
	u64 stable_state_hash(std::vector<u8> const &bytes) const;

	running_machine &m_machine;
	std::vector<snapshot_slot> m_snapshots;
	size_t m_snapshot_size = 0;
	u32 m_snapshot_capacity = 0;
	u32 m_snapshot_signature = 0;
	u32 m_save_count = 0;
	u32 m_load_count = 0;
	u32 m_discard_count = 0;
	u32 m_last_snapshot_size = 0;
	u64 m_save_time_us = 0;
	u64 m_load_time_us = 0;
	u64 m_discard_time_us = 0;
	bool m_trace = false;
};

#endif // MAME_FRONTEND_MAME_KAILERON_STATE_STORE_H
