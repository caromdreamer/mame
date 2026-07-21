// license:BSD-3-Clause

#include "emu.h"
#include "main.h"

#include "kaileron_frame_runner.h"

#include <chrono>
#include <cstdlib>

namespace {

u32 advance_guard_limit()
{
	char const *value = std::getenv("KN_MAME_ADVANCE_GUARD");
	if (!value || !value[0])
		return 1'000'000;

	char *end = nullptr;
	unsigned long const parsed = std::strtoul(value, &end, 10);
	if ((end && *end) || parsed > 0xffffffffUL)
		return 1'000'000;
	return u32(parsed);
}

u64 elapsed_us(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - start).count();
}

class scoped_kaileron_frame
{
public:
	explicit scoped_kaileron_frame(kaileron_frame_request request) :
		m_previous_mode(g_kn_mame_frame_mode.load(std::memory_order_relaxed)),
		m_previous_hide_video(g_kn_mame_hide_video)
	{
		g_kn_mame_frame_mode.store(request.mode, std::memory_order_relaxed);
		g_kn_mame_hide_video = !request.present_video;
	}

	~scoped_kaileron_frame()
	{
		g_kn_mame_frame_mode.store(m_previous_mode, std::memory_order_relaxed);
		g_kn_mame_hide_video = m_previous_hide_video;
	}

private:
	kaileron_frame_mode const m_previous_mode;
	bool const m_previous_hide_video;
};

} // anonymous namespace

char const *kaileron_frame_mode_name(kaileron_frame_mode mode) noexcept
{
	switch (mode)
	{
	case kaileron_frame_mode::authoritative:
		return "authoritative";
	case kaileron_frame_mode::rollback_resimulation:
		return "rollback_resimulation";
	case kaileron_frame_mode::spectator_catchup:
		return "spectator_catchup";
	case kaileron_frame_mode::speculative_presentation:
		return "speculative_presentation";
	}
	return "unknown";
}

kaileron_frame_runner::kaileron_frame_runner(running_machine &machine) :
	m_machine(machine)
{
}

bool kaileron_frame_runner::advance(u32 sdk_frame, kaileron_frame_request request)
{
	scoped_kaileron_frame const frame_scope(request);
	u32 const target_completed_frames = m_completed_frame_count + 1;
	u32 const guard_limit = advance_guard_limit();
	u32 guard = 0;
	auto const start = std::chrono::steady_clock::now();

	if (m_trace)
		osd_printf_info(
				"Kaileron trace: frame begin sdk_frame=%u mode=%s present_video=%u completed=%u\n",
				sdk_frame,
				kaileron_frame_mode_name(request.mode),
				request.present_video ? 1 : 0,
				m_completed_frame_count);

	while (m_completed_frame_count < target_completed_frames && !m_machine.scheduled_event_pending())
	{
		m_machine.scheduler().timeslice();
		if (++guard > guard_limit)
		{
			if (m_trace)
				osd_printf_info(
						"Kaileron trace: frame guard exhausted sdk_frame=%u mode=%s completed=%u target=%u guard=%u\n",
						sdk_frame,
						kaileron_frame_mode_name(request.mode),
						m_completed_frame_count,
						target_completed_frames,
						guard);
			return false;
		}
	}

	m_advance_count++;
	m_advance_time_us += elapsed_us(start);
	if (m_trace)
		osd_printf_info(
				"Kaileron trace: frame done sdk_frame=%u mode=%s completed=%u advances=%u\n",
				sdk_frame,
				kaileron_frame_mode_name(request.mode),
				m_completed_frame_count,
				m_advance_count);
	return true;
}

void kaileron_frame_runner::frame_done(kaileron_frame_mode mode) noexcept
{
	m_completed_frame_count++;
	if (mode == kaileron_frame_mode::rollback_resimulation)
		m_rollback_resimulation_count++;
	else if (mode == kaileron_frame_mode::spectator_catchup)
		m_spectator_catchup_count++;
	else if (mode == kaileron_frame_mode::speculative_presentation)
		m_speculative_presentation_count++;
}

u64 kaileron_frame_runner::average_advance_us() const noexcept
{
	return m_advance_count ? m_advance_time_us / m_advance_count : 0;
}
