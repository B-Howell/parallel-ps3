/* An lv2 event queue: its events and the threads waiting for one, with no
 * lock and no turn. C89.
 *
 * The events are a bounded ring (lv2_event_ring.h). The waiters are a bit
 * each in two 128-bit words - a PPU thread's slot is its ID's index, an SPU
 * thread's its group's (one thread of a group waits at a time: its group
 * waits with it) - with the ticket each took as it joined, for the order
 * they came in. A receive that finds no event joins and then looks at the
 * ring again; a send puts its event in the ring and then looks at the
 * waiters - with a sequentially consistent fence between, so one of the
 * two sees the other.
 *
 * A send that finds waiters claims the best (by the caller's rank, then in
 * the order they came: its bit cleared, it is the send's) and takes the
 * oldest event out of the ring for it, for the caller to give it (its
 * registers, its mailbox), then tells it its verdict and wakes it. When a
 * receive took the event meanwhile, the waiter is put back in its place.
 * A waiter whose time runs out while a send has it claimed waits for its
 * verdict - its time-out, or the event.
 *
 * Nothing waits for anything but a waiter for its own verdict, and a
 * thread that asks to be told when the queue changes (lv2_evq_watch). */

#ifndef LV2_EVQ_H
#define LV2_EVQ_H

#include <stdint.h>

#include <retro_atomic.h>

#include "lv2_event_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LV2_EVQ_SLOTS 256u

/* A waiter's verdict */
#define LV2_EVQ_PENDING  0
#define LV2_EVQ_TAKEN    1   /* given an event */
#define LV2_EVQ_CANCELED 2   /* the queue destroyed */
#define LV2_EVQ_TIMEDOUT 3

typedef struct lv2_evq
{
   lv2_event_ring_t   ring;
   retro_atomic_128_t waiting[2];               /* slots 0-127, 128-255 */
   retro_atomic_int_t next_ticket;
   retro_atomic_int_t ticket[LV2_EVQ_SLOTS];
   retro_atomic_int_t timed_out[LV2_EVQ_SLOTS];
   retro_atomic_int_t verdict[LV2_EVQ_SLOTS];
   /* Sends with a waiter claimed and not yet given its verdict */
   retro_atomic_int_t claiming;
   /* Bumped as threads join and events are taken, for watchers */
   retro_atomic_int_t changes;
   retro_atomic_int_t watchers;
} lv2_evq_t;

/* A waiter's rank, the lower the sooner it is given an event; NULL: in the
 * order they came */
typedef int (*lv2_evq_rank_t)(void *ctx, unsigned slot);

/* Room for @size events: 0 when it cannot be had */
int lv2_evq_init(lv2_evq_t *q, int size);
void lv2_evq_deinit(lv2_evq_t *q);

/* The oldest event taken into @ev: 0 when there is none */
int lv2_evq_try(lv2_evq_t *q, uint64_t ev[4]);

#define LV2_EVQ_WAITING 0
#define LV2_EVQ_GOT     1
#define LV2_EVQ_BAD    -1   /* slot out of range, or waiting already */

/* The thread in @slot waits for an event: GOT (in @ev) or WAITING - it is
 * a waiter, or a send claimed it already and is giving it one */
int lv2_evq_wait(lv2_evq_t *q, unsigned slot, uint64_t ev[4]);

/* A waiter that leaves before it is given one: 1. 0 when it was claimed:
 * its verdict comes */
int lv2_evq_leave(lv2_evq_t *q, unsigned slot);

/* The waiter's time ran out: out of the waiters (TIMEDOUT), or claimed,
 * and given what the claim gives it - waited for here */
int lv2_evq_timeout(lv2_evq_t *q, unsigned slot);

/* A waiter's verdict as it stands, and waited for (its time-out told
 * first, so the one giving it wakes it) */
int lv2_evq_verdict(lv2_evq_t *q, unsigned slot);
int lv2_evq_await(lv2_evq_t *q, unsigned slot);

#define LV2_EVQ_FULL   -1
#define LV2_EVQ_SENT    0   /* in the ring, or taken from it by a receive */
#define LV2_EVQ_HANDED  1   /* @slot claimed, the event for it in @ev */

/* @ev sent: in the ring, then to the best waiter if there is one - claimed
 * (@slot) with the oldest event taken for it (@ev, which may be another
 * send's: they are in order), for the caller to give it, then
 * lv2_evq_give. FULL: the queue holds its size, nothing changed */
int lv2_evq_send(lv2_evq_t *q, uint64_t ev[4], lv2_evq_rank_t rank, void *ctx, unsigned *slot);

/* The verdict given to the waiter a send or a cancel claimed, last before
 * it is woken */
void lv2_evq_give(lv2_evq_t *q, unsigned slot, int verdict);

/* Every waiter claimed at once (a destroy): their slots' bits, 0-127 in
 * @woken[0]; the count back. Each is to be given its verdict */
unsigned lv2_evq_claim_all(lv2_evq_t *q, retro_u128_t woken[2]);

/* Events held; whether any thread waits, or @slot does; whether a send has
 * one claimed */
int lv2_evq_count(lv2_evq_t *q);
int lv2_evq_any_waiter(lv2_evq_t *q);
int lv2_evq_waits(lv2_evq_t *q, unsigned slot);
int lv2_evq_busy(lv2_evq_t *q);

/* Every event taken and dropped */
void lv2_evq_drain(lv2_evq_t *q);

/* The events in order, with nothing else using the queue */
void lv2_evq_each(lv2_evq_t *q, lv2_event_ring_each_t fn, void *ctx);

/* A thread waiting for the queue to change - a thread to join, an event to
 * be taken: the count to wait on as it stands (lv2_evq_watch), the wait
 * (lv2_evq_watch_wait, until it moves or @us microseconds pass; negative
 * for no bound), and done watching (lv2_evq_unwatch) */
unsigned lv2_evq_watch(lv2_evq_t *q);
void lv2_evq_watch_wait(lv2_evq_t *q, unsigned seen, int64_t us);
void lv2_evq_unwatch(lv2_evq_t *q);

#ifdef __cplusplus
}
#endif

#endif
