/* The threads waiting on an lv2 sync object (a semaphore, a mutex, an
 * event queue...), joined, left and woken from any thread with no lock and
 * no thread waiting for another. C89.
 *
 * In place of a linked list kept in the object's lock: a 128-bit word with
 * a bit for each thread slot (a PPU thread's slot is its ID's index, under
 * 100), and the ticket each waiter took from the object as it joined, for
 * the order threads joined in. Joining sets the slot's bit, leaving (a
 * timeout) clears it, and waking one clears the bit of the best waiter -
 * by the caller's ranking (a thread's priority), then by ticket - with one
 * compare-and-swap. The bit is what decides: a waker that clears it owns
 * the waking of that thread, and a thread that clears its own has left; a
 * thread whose bit someone else cleared was woken, and stays to be given
 * what it was woken for. Two wakers never take the same thread, and a
 * thread never both leaves and is woken.
 *
 * Pairing with the object's own state (its count, its events) is the
 * caller's, in the usual way: a waiter joins and then looks at the state
 * again; a waker changes the state and then looks for waiters; both with
 * a sequentially consistent fence between (lv2_waitset_join and
 * lv2_waitset_fence give one), so at least one of them sees the other. */

#ifndef LV2_WAITSET_H
#define LV2_WAITSET_H

#include <stdint.h>

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV2_WAITSET_SLOTS 128u

typedef struct lv2_waitset
{
   retro_atomic_128_t waiting;                     /* bit per slot */
   retro_atomic_int_t next_ticket;
   retro_atomic_int_t ticket[LV2_WAITSET_SLOTS];   /* the waiter's, while its bit is set */
} lv2_waitset_t;

/* A waiter's rank, the lower the sooner woken: its priority, or 0 for all
 * when the object wakes in the order threads came */
typedef int (*lv2_waitset_rank_t)(void *ctx, unsigned slot);

void lv2_waitset_init(lv2_waitset_t *ws);

/* The thread in @slot waits from now: its bit set after its ticket. A
 * sequentially consistent fence follows, for the waiter to look at the
 * object's state again after it. 0 when the slot is out of range or its
 * bit is set already */
int lv2_waitset_join(lv2_waitset_t *ws, unsigned slot);

/* The thread in @slot stops waiting (timed out, or found what it waited
 * for after joining). 1 when it left; 0 when a waker had claimed it
 * already - it was woken, and is to be given what it was woken for */
int lv2_waitset_leave(lv2_waitset_t *ws, unsigned slot);

/* A waker that claimed @slot and found it had nothing for it after all
 * puts it back, with the ticket it joined with: its place in the order
 * kept. A sequentially consistent fence follows, as for a join. 0 when
 * the slot is out of range or its bit is set already */
int lv2_waitset_rejoin(lv2_waitset_t *ws, unsigned slot);

/* The best waiter claimed for waking: its slot, or -1 when none waits.
 * @rank NULL: in the order they joined */
int lv2_waitset_claim(lv2_waitset_t *ws, lv2_waitset_rank_t rank, void *ctx);

/* Every waiter claimed at once (a destroy, a broadcast): their bits, the
 * low 64 slots in lo */
retro_u128_t lv2_waitset_claim_all(lv2_waitset_t *ws);

/* Whether @slot waits, and whether any does: as of the load */
int lv2_waitset_has(lv2_waitset_t *ws, unsigned slot);
int lv2_waitset_any(lv2_waitset_t *ws);

/* The waker's side of the pairing: after changing the object's state,
 * before looking for waiters */
void lv2_waitset_fence(void);

#ifdef __cplusplus
}
#endif

#endif
