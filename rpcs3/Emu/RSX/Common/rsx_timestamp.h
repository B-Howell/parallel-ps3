/* The RSX's report timestamps: strictly rising, and never waited for.
 *
 * A report's time is the guest's, in nanoseconds. Two reports in one tick
 * of the guest's clock used to get the tick plus 2, 4, ... and, past what
 * fits in a tick, the RSX spun until the clock moved on. Now a report gets
 * the clock, or 2 past the last one given when that is later: a burst runs
 * a few nanoseconds ahead of the clock, which catches up, and nothing
 * waits. Plain memory: the RSX thread's alone. */

#ifndef RSX_TIMESTAMP_H
#define RSX_TIMESTAMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The next timestamp, @now being the guest's clock; @last the one given
 * before (0 at first), moved on */
uint64_t rsx_timestamp_next(uint64_t *last, uint64_t now);

#ifdef __cplusplus
}
#endif

#endif
