// license:BSD-3-Clause
// Browser callback timing is presentation policy, not emulation state.
#ifndef MAME_EMU_KAILERON_WEB_PACING_H
#define MAME_EMU_KAILERON_WEB_PACING_H

#include <cmath>
#include <algorithm>

class kaileron_web_pacer
{
public:
	static constexpr unsigned MAX_CATCHUP = 4;

	void configure(double period_ms)
	{
		m_period_ms = std::isfinite(period_ms) && period_ms > 0 ? period_ms : 1000.0 / 60.0;
		reset();
	}

	void reset() { m_started = false; }

	double next_delay_ms(double now_ms) const
	{
		if (!m_started || !std::isfinite(now_ms) || now_ms < m_last_ms)
			return 0;
		return std::max(0.0, m_deadline_ms - now_ms);
	}

	unsigned steps(double now_ms)
	{
		if (!m_started || now_ms < m_last_ms)
		{
			m_started = true;
			m_last_ms = now_ms;
			m_deadline_ms = now_ms + m_period_ms;
			return 1;
		}
		m_last_ms = now_ms;
		if (now_ms + 1e-6 < m_deadline_ms)
			return 0;

		double const due = 1 + std::floor((now_ms + 1e-6 - m_deadline_ms) / m_period_ms);
		if (due > MAX_CATCHUP)
		{
			// Drop excessive wall-clock debt after a suspended tab or long task.
			// Never skip emulated frames or exhaust a callback with catch-up work.
			m_deadline_ms = now_ms + m_period_ms;
			return MAX_CATCHUP;
		}
		m_deadline_ms += due * m_period_ms;
		return unsigned(due);
	}

private:
	double m_period_ms = 1000.0 / 60.0;
	double m_deadline_ms = 0;
	double m_last_ms = 0;
	bool m_started = false;
};

#endif
