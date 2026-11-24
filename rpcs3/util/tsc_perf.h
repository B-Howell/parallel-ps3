#ifndef TSC_PERF_H__
#define TSC_PERF_H__

/* The TSC's frequency as Linux has it, read from a perf event's page
 * (perf_event_mmap_page: time_mult, time_shift), so that it need not be
 * measured where CPUID does not state it (tsc_cpuid.h), as on AMD:
 * measuring it slept the loading thread 50 ms between two reads of the
 * wall clock. The kernel fills the page in only where it reads time from
 * the TSC itself (cap_user_time: a native, stable sched_clock, so not in
 * a VM), and gives the page only where perf_event_paranoid lets a thread
 * watch itself. Not on Android, whose app filter kills a process making
 * a system call it does not allow rather than failing it. C89. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The frequency from the kernel's conversion of TSC ticks to ns,
 * ns = ticks * mult >> shift: 10^9 << shift / mult, rounded; 0 for a
 * mult of 0 or a shift too wide to work it out in 64 bits */
uint64_t tsc_perf_freq_from(uint32_t mult, uint32_t shift);

/* Read from a perf event's page made and let go of at once: 0 where
 * there is no page or the kernel does not fill it in (not Linux or
 * Android, too) */
uint64_t tsc_perf_freq(void);

#ifdef __cplusplus
}
#endif

#endif
