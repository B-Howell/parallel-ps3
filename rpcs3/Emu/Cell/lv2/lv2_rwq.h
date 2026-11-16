/* An lv2 rwlock's owners and the threads waiting for it, with no lock:
 * taking it to read or to write, queueing for it, leaving the queue on a
 * timeout and giving it up - to the best writer waiting, or to every
 * reader waiting - with compare-and-swap. No thread waits for another.
 * C89.
 *
 * The owners and the writers waiting are one 128-bit word:
 *
 *   lo:             a bit for each writer waiting, slots 0 to 63
 *   hi bits 0-35:   a bit for each writer waiting, slots 64 to 99
 *   hi bits 36-55:  the readers that hold it
 *   hi bits 56-62:  the writer's slot plus one; 0 when none
 *
 * The readers waiting are a waiter set (lv2_waitset.h) of their own: in
 * the word with the writers, a slot that left and queued again for the
 * other could leave the word as a waker had seen it, and be given the
 * wrong lock. A PPU thread's slot is its ID's index (idm has room for
 * 100); each writer waiting has a ticket, for the order threads came in.
 *
 * As LV2 has it: a reader takes it at once only when no writer holds it
 * or waits for it; a writer that gives it up hands it to the best writer
 * waiting, else to every reader waiting; the last reader to give it up
 * hands it to the best writer waiting; and when the last writer waiting
 * times out, the readers waiting are given it. Readers are given it in
 * three changes: the word counts as many readers as there are slots
 * (holding writers off), every reader waiting is taken off its set, and
 * the word counts those taken. A reader that queues looks at the word
 * again after it joined, and takes it itself when nothing holds it off. */

#ifndef LV2_RWQ_H
#define LV2_RWQ_H

#include <stdint.h>

#include <retro_atomic.h>

#include "lv2_waitset.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LV2_RWQ_SLOTS 100u
#define LV2_RWQ_MAX_READERS 0xfffffu

typedef struct lv2_rwq
{
   retro_atomic_128_t word;
   retro_atomic_int_t next_ticket;
   retro_atomic_int_t ticket[LV2_RWQ_SLOTS];  /* a writer's, while it waits */
   lv2_waitset_t      readers;                /* the readers waiting */
} lv2_rwq_t;

/* A waiting writer's rank, the lower the sooner it is given the lock (its
 * priority); NULL: in the order they came */
typedef int (*lv2_rwq_rank_t)(void *ctx, unsigned slot);

/* @writer: a slot, or -1 for none; @readers: those that hold it (none when
 * a writer does) */
void lv2_rwq_init(lv2_rwq_t *q, int writer, unsigned readers);

#define LV2_RWQ_QUEUED  0
#define LV2_RWQ_OWNED   1
#define LV2_RWQ_MINE    2   /* the caller writes already: nothing changed */
#define LV2_RWQ_BUSY    3   /* nothing changed */
#define LV2_RWQ_BAD    -1   /* slot out of range, queued already, or too many readers */

/* Read now if it can be (OWNED), else BUSY */
int lv2_rwq_tryrlock(lv2_rwq_t *q);

/* Read now (OWNED), else the slot queues to read (QUEUED): it waits, or
 * was given it already by an unlock that is waking it */
int lv2_rwq_rlock(lv2_rwq_t *q, unsigned slot);

/* Written now when free (OWNED); MINE, or BUSY */
int lv2_rwq_trywlock(lv2_rwq_t *q, unsigned slot);

/* Written now when free (OWNED); MINE; else the slot queues to write */
int lv2_rwq_wlock(lv2_rwq_t *q, unsigned slot);

#define LV2_RWQ_NOT_OWNER -2

/* A reader gives it up, a writer gives it up: 0, and in @woken the slots
 * given it (one writer, or readers; the low 64 in lo) for the caller to
 * wake. NOT_OWNER when no reader holds it, or @slot does not write:
 * nothing changed */
int lv2_rwq_runlock(lv2_rwq_t *q, lv2_rwq_rank_t rank, void *ctx, retro_u128_t *woken);
int lv2_rwq_wunlock(lv2_rwq_t *q, unsigned slot, lv2_rwq_rank_t rank, void *ctx, retro_u128_t *woken);

/* Out of the queue (its timeout), a reader or a writer: 1, and in @woken
 * the readers that a writer leaving lets in. 0 when it was not queued -
 * it was given the lock */
int lv2_rwq_rleave(lv2_rwq_t *q, unsigned slot);
int lv2_rwq_wleave(lv2_rwq_t *q, unsigned slot, lv2_rwq_rank_t rank, void *ctx, retro_u128_t *woken);

/* The writer's slot (-1 when none), the readers (for a moment as many as
 * there are slots more, as readers are given it), whether @slot waits,
 * whether any does, and whether anything holds it or waits for it */
int lv2_rwq_writer(lv2_rwq_t *q);
unsigned lv2_rwq_readers(lv2_rwq_t *q);
int lv2_rwq_waits(lv2_rwq_t *q, unsigned slot);
int lv2_rwq_any_waiter(lv2_rwq_t *q);
int lv2_rwq_busy(lv2_rwq_t *q);

#ifdef __cplusplus
}
#endif

#endif
