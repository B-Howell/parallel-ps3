#pragma once

#include "util/types.hpp"

#include <features/features_cpu.h>

// A stopwatch on libretro-common's monotonic microsecond clock
// (cpu_features_get_time_usec): its times are plain microsecond counts
class Timer
{
private:
	bool m_stopped;
	u64 m_start;
	u64 m_end;

public:
	// Now, in microseconds, on the clock the timer reads
	static u64 now_us()
	{
		return static_cast<u64>(cpu_features_get_time_usec());
	}

	Timer() : m_stopped(false), m_start(now_us()), m_end(0)
	{
	}

	void Start()
	{
		m_stopped = false;
		m_start = now_us();
	}

	void Stop()
	{
		m_stopped = true;
		m_end = now_us();
	}

	double GetElapsedTimeInSec() const
	{
		return static_cast<double>(GetElapsedTimeInMicroSec()) / 1000000.0;
	}

	double GetElapsedTimeInMilliSec() const
	{
		return static_cast<double>(GetElapsedTimeInMicroSec()) / 1000.0;
	}

	u64 GetElapsedTimeInMicroSec() const
	{
		return (m_stopped ? m_end : now_us()) - m_start;
	}

	// Milliseconds since @timestamp_us (a now_us() reading)
	u64 GetMsSince(u64 timestamp_us) const
	{
		return ((m_stopped ? m_end : now_us()) - timestamp_us) / 1000;
	}
};
