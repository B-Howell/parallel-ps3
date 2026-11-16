/* An lv2 event flag: its bits and the threads waiting on them, with no
 * lock and no turn. C89.
 *
 * The bits are one 64-bit word, taken and cleared with compare-and-swap.
 * The waiters are a waiter set (lv2_waitset.h), each with the bits it
 * waits for and how (AND or OR, clearing them or all or none). A thread
 * waits by joining and then looking at the bits again; a set ORs its bits
 * in and then looks at the waiters - with a sequentially consistent fence
 * between, so one of the two sees the other.
 *
 * A set wakes the waiters it satisfies best first (by the caller's rank,
 * then in the order they came), each in two changes: it claims the waiter
 * (clears its bit; the thread is then the set's to give a result to) and
 * then takes the bits for it. When the bits went meanwhile (another
 * waiter, a clear), the waiter is put back with its place in the order
 * (lv2_waitset_rejoin), and looked at again. A waiter whose time ran out
 * while it was claimed is given its time-out by the one that claimed it.
 *
 * Every result - the bits taken, cancelled, timed out - is given in the
 * waiter's slot: its code (a verdict, nonzero once given) and the
 * pattern. A thread reads its own after it is woken, and only after the
 * verdict, which is the last thing a waker writes for it before it wakes
 * it. Nothing waits for anything but the thread for its own result. */

#ifndef LV2_FLAGQ_H
#define LV2_FLAGQ_H

#include <stdint.h>

#include <retro_atomic.h>

#include "lv2_waitset.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LV2_FLAGQ_SLOTS LV2_WAITSET_SLOTS

/* The modes, as LV2 has them */
#define LV2_FLAGQ_AND       0x01u
#define LV2_FLAGQ_OR        0x02u
#define LV2_FLAGQ_CLEAR     0x10u
#define LV2_FLAGQ_CLEAR_ALL 0x20u

/* A waiter's verdict */
#define LV2_FLAGQ_PENDING   0
#define LV2_FLAGQ_TAKEN     1
#define LV2_FLAGQ_CANCELED  2
#define LV2_FLAGQ_TIMEDOUT  3

typedef struct lv2_flagq
{
   retro_atomic_64_t pattern;
   lv2_waitset_t     ws;
   /* Sets and cancels with a waiter claimed and not yet given its result
    * (a destroy is refused meanwhile) */
   retro_atomic_int_t claiming;
   retro_atomic_64_t  want[LV2_FLAGQ_SLOTS];
   retro_atomic_int_t mode[LV2_FLAGQ_SLOTS];
   retro_atomic_int_t timed_out[LV2_FLAGQ_SLOTS];
   retro_atomic_int_t verdict[LV2_FLAGQ_SLOTS];
   retro_atomic_64_t  result[LV2_FLAGQ_SLOTS];
} lv2_flagq_t;

/* A waiter's rank, the lower the sooner it is woken; NULL: in the order
 * they came */
typedef int (*lv2_flagq_rank_t)(void *ctx, unsigned slot);

void lv2_flagq_init(lv2_flagq_t *q, uint64_t pattern);

/* Whether @mode is one LV2 has */
int lv2_flagq_mode_ok(unsigned mode);

/* The bits taken now if they are there: 1 and the pattern they were taken
 * from in @result (set either way) */
int lv2_flagq_try(lv2_flagq_t *q, uint64_t want, unsigned mode, uint64_t *result);

#define LV2_FLAGQ_WAITING  0
#define LV2_FLAGQ_GOT      1
#define LV2_FLAGQ_PERM    -1   /* @single and another waits already */
#define LV2_FLAGQ_BAD     -2   /* slot out of range, or waiting already */

/* The thread in @slot waits for @want: GOT (the bits taken now, the
 * pattern in @result) or WAITING - it is a waiter, or was claimed already
 * by a set or a cancel that gives it its result (lv2_flagq_verdict) and
 * then wakes it. @single: a flag that has one waiter at most */
int lv2_flagq_wait(lv2_flagq_t *q, unsigned slot, uint64_t want, unsigned mode,
      int single, uint64_t *result);

/* A waiter that leaves before it is woken (no such flag after all): 1. 0
 * when it was claimed: its result comes */
int lv2_flagq_leave(lv2_flagq_t *q, unsigned slot);

/* The waiter's time ran out: it is out of the waiters, with TIMEDOUT and
 * the pattern as it is, or, claimed by a set or a cancel, given what that
 * gives it - waited for here, as the one who claimed it is giving it. The
 * verdict comes back; the pattern is lv2_flagq_result */
int lv2_flagq_timeout(lv2_flagq_t *q, unsigned slot);

/* A waiter's verdict and pattern, once woken. The thread whose result is
 * not given yet when it must stop (the emulator stopping) waits for it */
int lv2_flagq_verdict(lv2_flagq_t *q, unsigned slot);
int lv2_flagq_await(lv2_flagq_t *q, unsigned slot);
uint64_t lv2_flagq_result(lv2_flagq_t *q, unsigned slot);

/* @bits ORed in, then every waiter it satisfies taken: their slots in
 * @woken (the low 64 in lo), for the caller to wake. The waiters are given
 * their verdicts before they are in @woken */
void lv2_flagq_set(lv2_flagq_t *q, uint64_t bits, lv2_flagq_rank_t rank, void *ctx,
      retro_u128_t *woken);

/* Every waiter cancelled, with the pattern as it is: their slots in
 * @woken; the count back */
unsigned lv2_flagq_cancel(lv2_flagq_t *q, retro_u128_t *woken);

/* The bits ANDed with @bits */
void lv2_flagq_clear(lv2_flagq_t *q, uint64_t bits);

uint64_t lv2_flagq_pattern(lv2_flagq_t *q);

/* Whether anything waits, or is being given a result: no destroy then */
int lv2_flagq_busy(lv2_flagq_t *q);

#ifdef __cplusplus
}
#endif

#endif
