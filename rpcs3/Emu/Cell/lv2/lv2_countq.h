/* An lv2 semaphore's state: its count, or the threads waiting for a count,
 * in one 128-bit word, so that taking a count, joining the waiters,
 * leaving them on a timeout and posting (and choosing whom a post wakes)
 * are each one compare-and-swap. No lock, and no thread waits for another;
 * nothing can happen between a thread finding no count and its joining
 * the waiters, as both are the one change. C89.
 *
 * A count and waiters never coexist: a thread waits only when there is no
 * count, and a post gives to waiters before it adds to the count. So the
 * word holds one or the other:
 *
 *   bit 127 set:    a count, in the low 64 bits (the whole of an s32 and
 *                   more, whatever the semaphore's most); no waiters
 *   bit 127 clear:  a bit for each thread slot that waits (slots 0 to
 *                   126); no count. All clear: neither
 *
 * A PPU thread's slot is its ID's index (under 100: idm has room for 100
 * PPU threads); a waiter's ticket, taken as it joins, keeps the order the
 * threads came in, for a FIFO semaphore and among equals of a priority
 * one. */

#ifndef LV2_COUNTQ_H
#define LV2_COUNTQ_H

#include <stdint.h>

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV2_COUNTQ_SLOTS 127u

typedef struct lv2_countq
{
   retro_atomic_128_t word;
   retro_atomic_int_t next_ticket;
   retro_atomic_int_t ticket[LV2_COUNTQ_SLOTS];
} lv2_countq_t;

/* A waiter's rank, the lower the sooner woken (its priority); NULL: in the
 * order they came */
typedef int (*lv2_countq_rank_t)(void *ctx, unsigned slot);

void lv2_countq_init(lv2_countq_t *q, uint64_t count);

/* A count taken if there is one: 1; else 0, nothing changed */
int lv2_countq_trywait(lv2_countq_t *q);

/* A count taken if there is one: 1. Else the thread in @slot joins the
 * waiters, in the same change: 0. -1 for a slot out of range or waiting
 * already */
int lv2_countq_wait(lv2_countq_t *q, unsigned slot);

/* The waiter in @slot leaves (its timeout): 1. 0 when a post chose it
 * first: it was given a count, and is being woken */
int lv2_countq_leave(lv2_countq_t *q, unsigned slot);

#define LV2_COUNTQ_POSTED 1
#define LV2_COUNTQ_BUSY   0  /* over the most: nothing changed */

/* @count given: to as many waiters as there are, the best first, the rest
 * added to the count, which may come to @max at most beyond the waiters
 * given one. The waiters given a count are cleared from the word in the
 * same change and set in *@woken (slot n in bit n, the low 64 in lo), for
 * the caller to wake */
int lv2_countq_post(lv2_countq_t *q, uint64_t count, uint64_t max,
      lv2_countq_rank_t rank, void *ctx, retro_u128_t *woken);

/* The count, 0 when threads wait or there is none */
uint64_t lv2_countq_value(lv2_countq_t *q);

/* Whether @slot waits, and whether any thread does */
int lv2_countq_waits(lv2_countq_t *q, unsigned slot);
int lv2_countq_any_waiter(lv2_countq_t *q);

#ifdef __cplusplus
}
#endif

#endif
