#ifndef CPU_GUARD_H__
#define CPU_GUARD_H__

/* What suspend_all, the threads it suspends and the walkers of their list
 * keep to, with no lock (it was a shared_mutex, s_cpu_lock). C89.
 *
 * - epoch: odd while a suspend runs. One suspend at a time: the next
 *   waits for the last to end (cpu_guard_suspend_begin).
 * - A thread entering the list publishes itself, then waits out a suspend
 *   running then (cpu_guard_wait_suspend): that suspend either saw it, or
 *   began before it was there and must not have it run meanwhile. One
 *   that begins after sees it. Each side's store before its load, with a
 *   full fence between, so the one or the other sees the other.
 * - A thread leaving the list takes itself out, then waits out the
 *   suspend running then and the walkers in the list then
 *   (cpu_guard_retire), as either may hold a pointer to it read before.
 * - A walker of the list outside a suspend (cpu_guard_read_begin, _end)
 *   only stops a thread leaving from going on while it walks.
 *
 * The waits are on these words changing, woken by the one that changes
 * them where someone waits: no spin, no clock. */

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cpu_guard
{
   retro_atomic_int_t epoch;
   retro_atomic_int_t epoch_waiters;
   retro_atomic_int_t readers;
   retro_atomic_int_t readers_waiters;
} cpu_guard_t;

void cpu_guard_suspend_begin(cpu_guard_t *g);
void cpu_guard_suspend_end(cpu_guard_t *g);

/* Until a suspend running now is over (at once where none is) */
void cpu_guard_wait_suspend(cpu_guard_t *g);

void cpu_guard_read_begin(cpu_guard_t *g);
void cpu_guard_read_end(cpu_guard_t *g);

/* A thread out of the list: until no suspend or walker can be on it */
void cpu_guard_retire(cpu_guard_t *g);

#ifdef __cplusplus
}
#endif

#endif
