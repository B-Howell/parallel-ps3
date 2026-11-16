/* An lv2 mutex's owner and the threads waiting for it, in one 128-bit word,
 * so that taking it, queueing for it, leaving the queue on a timeout,
 * giving it up to the best waiter and handing it to a thread a condition
 * wakes are each one compare-and-swap. No lock, and no thread waits for
 * another. C89.
 *
 *   lo:            a bit for each waiting slot 0 to 63
 *   hi bits 0-35:  a bit for each waiting slot 64 to 99
 *   hi bits 56-62: the owner's slot plus one; 0 when free
 *
 * A PPU thread's slot is its ID's index (idm has room for 100). Each
 * waiter's ticket, taken as it queues, keeps the order threads came in,
 * for a FIFO mutex and among equals of a priority one. The owner may be
 * queued too: a condition that wakes its own mutex's owner queues it, and
 * the owner's unlock may then hand the mutex back to it. */

#ifndef LV2_OWNQ_H
#define LV2_OWNQ_H

#include <stdint.h>

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV2_OWNQ_SLOTS 100u

typedef struct lv2_ownq
{
   retro_atomic_128_t word;
   retro_atomic_int_t next_ticket;
   retro_atomic_int_t ticket[LV2_OWNQ_SLOTS];
} lv2_ownq_t;

/* A waiter's rank, the lower the sooner it is given the mutex (its
 * priority); NULL: in the order they came */
typedef int (*lv2_ownq_rank_t)(void *ctx, unsigned slot);

/* @owner: a slot, or -1 for none */
void lv2_ownq_init(lv2_ownq_t *q, int owner);

#define LV2_OWNQ_QUEUED  0
#define LV2_OWNQ_OWNED   1
#define LV2_OWNQ_MINE    2   /* the caller owns it already: nothing changed */
#define LV2_OWNQ_BUSY    3   /* another owns it: nothing changed */
#define LV2_OWNQ_BAD    -1   /* slot out of range, or queued already */

/* Owned now when free (OWNED), else MINE or BUSY */
int lv2_ownq_trylock(lv2_ownq_t *q, unsigned slot);

/* Owned now when free (OWNED); MINE when the caller owns it; else the
 * slot queues, in the same change (QUEUED) */
int lv2_ownq_lock(lv2_ownq_t *q, unsigned slot);

/* For a thread a condition wakes: owned when free (OWNED), else queued
 * (QUEUED), whoever owns it - its own owner included */
int lv2_ownq_give(lv2_ownq_t *q, unsigned slot);

/* Out of the queue (its timeout): 1. 0 when it was not queued - an unlock
 * handed it the mutex first */
int lv2_ownq_leave(lv2_ownq_t *q, unsigned slot);

#define LV2_OWNQ_NOT_OWNER -2
#define LV2_OWNQ_FREED     -1

/* The owner in @slot gives it up: to the best waiter, whose slot it gives
 * back for the caller to wake, or freed (FREED) when none waits. NOT_OWNER
 * when @slot does not own it: nothing changed */
int lv2_ownq_unlock(lv2_ownq_t *q, unsigned slot, lv2_ownq_rank_t rank, void *ctx);

/* The owner's slot, -1 when free; whether a slot waits, and any */
int lv2_ownq_owner(lv2_ownq_t *q);
int lv2_ownq_waits(lv2_ownq_t *q, unsigned slot);
int lv2_ownq_any_waiter(lv2_ownq_t *q);

#ifdef __cplusplus
}
#endif

#endif
