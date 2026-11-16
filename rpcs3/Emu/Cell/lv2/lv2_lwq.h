/* An lv2 lightweight mutex's signal and the threads sleeping on it, in
 * one 128-bit word, so that taking the signal, queueing to sleep, leaving
 * the queue on a timeout and giving the lwmutex up - to the best sleeper,
 * or as the signal when none sleeps - are each one compare-and-swap. No
 * lock, and no thread waits for another. C89.
 *
 *   lo:            a bit for each sleeping slot 0 to 63
 *   hi bits 0-35:  a bit for each sleeping slot 64 to 99
 *   hi bit 62:     the signal an unlock leaves (LV2_LWQ_SIGNAL)
 *   hi bit 63:     the signal an unlock2 leaves (LV2_LWQ_SIGNAL2)
 *
 * The lwmutex's owner lives in guest memory; LV2 keeps only this: a
 * thread that finds the signal takes it, one that does not sleeps until
 * an unlock gives it the lwmutex. A PPU thread's slot is its ID's index
 * (idm has room for 100); each sleeper's ticket keeps the order threads
 * came in, for a FIFO lwmutex and among equals of the others. */

#ifndef LV2_LWQ_H
#define LV2_LWQ_H

#include <stdint.h>

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV2_LWQ_SLOTS 100u

/* The signal's bits, as LV2 keeps them in a 32-bit word */
#define LV2_LWQ_SIGNAL  0x00000001u
#define LV2_LWQ_SIGNAL2 0x80000000u

typedef struct lv2_lwq
{
   retro_atomic_128_t word;
   retro_atomic_int_t next_ticket;
   retro_atomic_int_t ticket[LV2_LWQ_SLOTS];
   retro_atomic_int_t given2[LV2_LWQ_SLOTS]; /* given it by an unlock2 */
} lv2_lwq_t;

/* A sleeper's rank, the lower the sooner it is given the lwmutex (its
 * priority); NULL: in the order they came */
typedef int (*lv2_lwq_rank_t)(void *ctx, unsigned slot);

/* @signal: LV2_LWQ_SIGNAL and LV2_LWQ_SIGNAL2 bits */
void lv2_lwq_init(lv2_lwq_t *q, uint32_t signal);

uint32_t lv2_lwq_signal(lv2_lwq_t *q);

/* The signal taken, whichever: its bits back, 0 when there was none */
uint32_t lv2_lwq_take(lv2_lwq_t *q);

/* The signal taken when an unlock left it (not an unlock2): 1 */
int lv2_lwq_trylock(lv2_lwq_t *q);

#define LV2_LWQ_QUEUED 0
#define LV2_LWQ_OWNED  1
#define LV2_LWQ_BAD   -1   /* slot out of range, or queued already */

/* The signal taken (OWNED, its bits in @signal), else the slot queues to
 * sleep, in the same change (QUEUED) */
int lv2_lwq_lock(lv2_lwq_t *q, unsigned slot, uint32_t *signal);

/* Given up: to the best sleeper, whose slot comes back for the caller to
 * wake; or, when none sleeps, the signal left (-1). @unlock2: the signal
 * an unlock2 leaves, and a sleeper given it by an unlock2 is told so
 * (lv2_lwq_given2), before it is out of the queue */
int lv2_lwq_unlock(lv2_lwq_t *q, int unlock2, lv2_lwq_rank_t rank, void *ctx);

/* Given up as the signal only when nobody sleeps: 1. 0 when somebody
 * does: nothing changed */
int lv2_lwq_unlock_free(lv2_lwq_t *q, int unlock2);

/* Out of the queue (its timeout): 1. 0 when it was not queued - an unlock
 * gave it the lwmutex first */
int lv2_lwq_leave(lv2_lwq_t *q, unsigned slot);

/* Whether the sleeper in @slot, given the lwmutex, was given it by an
 * unlock2 (LV2 then has it return EBUSY) */
int lv2_lwq_given2(lv2_lwq_t *q, unsigned slot);

int lv2_lwq_waits(lv2_lwq_t *q, unsigned slot);
int lv2_lwq_any_waiter(lv2_lwq_t *q);

#ifdef __cplusplus
}
#endif

#endif
