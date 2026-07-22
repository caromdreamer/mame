// license:BSD-3-Clause

#ifndef MAME_FRONTEND_MAME_KAILERON_FRAME_RUNNER_H
#define MAME_FRONTEND_MAME_KAILERON_FRAME_RUNNER_H

#pragma once

#include "emu.h"
#include "main.h"

struct kaileron_frame_request
{
	kaileron_frame_mode mode = kaileron_frame_mode::authoritative;
	bool present_video = true;

	static constexpr kaileron_frame_request authoritative() noexcept
	{
		return { kaileron_frame_mode::authoritative, true };
	}

	static constexpr kaileron_frame_request rollback_resimulation() noexcept
	{
		return { kaileron_frame_mode::rollback_resimulation, false };
	}

	static constexpr kaileron_frame_request spectator_catchup(bool present) noexcept
	{
		return { kaileron_frame_mode::spectator_catchup, present };
	}

	static constexpr kaileron_frame_request speculative_presentation(bool present) noexcept
	{
		return { kaileron_frame_mode::speculative_presentation, present };
	}
};

char const *kaileron_frame_mode_name(kaileron_frame_mode mode) noexcept;

class kaileron_frame_runner
{
public:
	explicit kaileron_frame_runner(running_machine &machine);

	void set_trace(bool trace) noexcept { m_trace = trace; }
	bool advance(u32 sdk_frame, kaileron_frame_request request);
	void frame_done(kaileron_frame_mode mode) noexcept;
	void frame_notifier() noexcept { m_frame_notifier_count++; }

	u32 completed_frame_count() const noexcept { return m_completed_frame_count; }
	u32 advance_count() const noexcept { return m_advance_count; }
	u32 frame_notifier_count() const noexcept { return m_frame_notifier_count; }
	u32 rollback_resimulation_count() const noexcept { return m_rollback_resimulation_count; }
	u32 spectator_catchup_count() const noexcept { return m_spectator_catchup_count; }
	u32 speculative_presentation_count() const noexcept { return m_speculative_presentation_count; }
	u64 average_advance_us() const noexcept;

private:
	running_machine &m_machine;
	u32 m_completed_frame_count = 0;
	u32 m_advance_count = 0;
	u32 m_frame_notifier_count = 0;
	u32 m_rollback_resimulation_count = 0;
	u32 m_spectator_catchup_count = 0;
	u32 m_speculative_presentation_count = 0;
	u64 m_advance_time_us = 0;
	bool m_trace = false;
};

#endif // MAME_FRONTEND_MAME_KAILERON_FRAME_RUNNER_H
