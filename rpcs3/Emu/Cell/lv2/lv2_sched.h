/* The lv2 scheduler's run queue with no lock and no turn. C89.
 *
 * What the scheduler knows - the runnable PPU threads in their order (by
 * priority, then as they came), each thread's priority, which threads were
 * told to give up their hardware thread and have not yet stopped, the
 * threads still to call their first sleep - is one snapshot. A change reads
 * the snapshot, works out the next one in a buffer of its own and puts it
 * in place with one compare-and-swap on a word naming the buffer and a
 * generation; a change that loses to another works it out again from the
 * newer one. So the changes are one after another, as under the turn they
 * were: each sees all the ones before it, and decides from them alone.
 *
 * A change decides who runs as the turn did: past the first @run_slots the
 * running threads are told to stop, and the threads let in are woken only
 * once every one told to stop has (lv2_sched_ack) - never more than
 * @run_slots on their hardware threads at once.
 *
 * A thread is let in or told to stop by an edge: its count of them, in the
 * snapshot, odd while it is let run. A change's edges are put to the thread
 * (lv2_sched_apply: the caller's flags and notify) once each, in order, by
 * whichever change finds the thread's next edge not yet put - never two at
 * once, and never an older edge after a newer, whatever order the changes
 * finish in.
 *
 * The buffers are set aside at init: nothing is allocated. A read that
 * a change overtakes is read again; nothing waits for anything. */

#ifndef LV2_SCHED_H
#define LV2_SCHED_H

#include <stdint.h>

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A PPU thread's slot is its ID's index */
#define LV2_SCHED_THREADS 100u
/* One for each change in flight - a thread makes one at a time, and only
 * PPU threads (100 at most) and the SPU, RSX and host threads waking them
 * make them - and the one in place. The buffer's number is 8 bits of the
 * word naming it */
#define LV2_SCHED_BUFFERS 256u

#define LV2_SCHED_LIST_WORDS ((LV2_SCHED_THREADS + 3u) / 4u)

typedef struct lv2_sched_buf
{
   retro_atomic_int_t head;      /* count, pending */
   retro_atomic_int_t others;    /* threads but PPU ones still to call their first sleep */
   retro_atomic_int_t list[LV2_SCHED_LIST_WORDS];
   retro_atomic_int_t thread[LV2_SCHED_THREADS];
} lv2_sched_buf_t;

typedef struct lv2_sched
{
   /* generation << 8 | the buffer in place */
   retro_atomic_64_t  cur;
   retro_atomic_64_t  free[(LV2_SCHED_BUFFERS + 63u) / 64u];
   /* Each thread's edges put to it: count << 1 | 1 while one is being put */
   retro_atomic_int_t applied[LV2_SCHED_THREADS];
   unsigned           run_slots;
   lv2_sched_buf_t    buf[LV2_SCHED_BUFFERS];
} lv2_sched_t;

/* A snapshot as read: plain memory, the caller's */
typedef struct lv2_sched_view
{
   unsigned      count;   /* runnable, in order in list */
   unsigned      pending; /* told to stop, not yet stopped */
   unsigned      others;
   unsigned char list[LV2_SCHED_THREADS];
   uint32_t      thread[LV2_SCHED_THREADS];
} lv2_sched_view_t;

/* A thread's word in a view */
#define LV2_SCHED_PRIO(w)     ((int)((w) & 0xfffu))
#define LV2_SCHED_QUEUED(w)   (((w) >> 12) & 1u)  /* runnable */
#define LV2_SCHED_TOLD(w)     (((w) >> 13) & 1u)  /* told to stop, not yet stopped */
#define LV2_SCHED_FIRST(w)    (((w) >> 14) & 1u)  /* still to call its first sleep */
#define LV2_SCHED_EDGES(w)    ((unsigned)((w) >> 16))
#define LV2_SCHED_LET_RUN(w)  (LV2_SCHED_EDGES(w) & 1u)

void lv2_sched_init(lv2_sched_t *s, unsigned run_slots);

/* Everything forgotten (the emulator stopped): nothing may be using it */
void lv2_sched_reset(lv2_sched_t *s);

/* The snapshot in place */
void lv2_sched_read(lv2_sched_t *s, lv2_sched_view_t *v);

/* What an edge does to the thread: let it run, or tell it to stop */
#define LV2_SCHED_RUN  1
#define LV2_SCHED_STOP 0

typedef void (*lv2_sched_edge_t)(void *ctx, unsigned slot, int edge);

/* Priority commands */
#define LV2_SCHED_YIELD   -1
#define LV2_SCHED_ENQUEUE -2

/* A change's ask and what came of it */
typedef struct lv2_sched_op
{
   /* sleep: the slot going to sleep, or -1 */
   int sleeper;
   /* sleep: the sleeper was signaled already: it is not to sleep */
   int signaled;

   /* awake: the slot, or -1 for the threads in @to_awake */
   int target;
   /* awake: a priority, LV2_SCHED_YIELD, LV2_SCHED_ENQUEUE, or nothing
    * to wake (an awake that is not asked: LV2_SCHED_NONE) */
   int prio;
   const unsigned char *to_awake;
   unsigned to_awake_count;

   /* The PPU slot making the change, or -1 */
   int caller;
   /* Whether threads may be let run yet (the scheduler made ready) */
   int ready;
   /* The caller saw its stop (its check of its state): told to stop, it
    * is taken as stopping. Else only once its stop is put to it */
   int stopped;

   /* What came of it */
   int result;              /* sleep_unlocked's, or awake_unlocked's */
   int changed;             /* the run queue changed (awake) */
   int lowest_new_prio;     /* of the threads put in it, else 0x7fffffff */
   int had_free_slot;       /* fewer than run_slots runnable before */
   int switched;            /* a yield went past the run slots */
   int prio_order;          /* a priority set: 1 to order it last, -1 first, 0 as it was */
   int last_first_sleep;    /* the last thread to call its first sleep did */
   uint32_t emplaced[4];    /* put in the run queue: their timeouts to drop */
   uint32_t found[4];       /* to be put in it, and in it already */
} lv2_sched_op_t;

#define LV2_SCHED_NONE -3

/* sleep_unlocked's results */
#define LV2_SCHED_SLEPT        1   /* out of the run queue */
#define LV2_SCHED_FIRST_SLEEP  2   /* its first sleep: taken off that list */
#define LV2_SCHED_NOT_QUEUED   0   /* not in the run queue, nor to call it */
#define LV2_SCHED_NOT_SLEPT   -1   /* signaled already */

/* The op, as one change, and its edges put to their threads with @edge */
void lv2_sched_change(lv2_sched_t *s, lv2_sched_op_t *op, lv2_sched_edge_t edge, void *ctx);

/* An op with nothing asked: for lv2_sched_change */
void lv2_sched_op_init(lv2_sched_op_t *op, int caller, int ready);

/* A thread to call its first sleep before the scheduler starts: a PPU slot
 * (marked in the snapshot), or another thread (@slot -1: counted) */
void lv2_sched_first_sleep(lv2_sched_t *s, int slot);

/* Another thread's first sleep, counted off: 1 when it was the last */
int lv2_sched_other_slept(lv2_sched_t *s);

/* Threads still to call their first sleep */
unsigned lv2_sched_first_sleeps(lv2_sched_t *s);

/* @slot's place in the run queue, or -1 */
int lv2_sched_place(lv2_sched_t *s, unsigned slot);

/* Runnable threads, and the slot @index places in */
unsigned lv2_sched_count(lv2_sched_t *s);
int lv2_sched_at(lv2_sched_t *s, unsigned index);

/* Any edge of @slot's not yet put, put (a change that saw another putting
 * one leaves the rest to it; this is for tests and dumps) */
void lv2_sched_apply(lv2_sched_t *s, unsigned slot, lv2_sched_edge_t edge, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
