/* The lv2 scheduler's run queue with no lock (lv2_sched.c): the order by
 * priority then as they came, a yield to the back of its equals, a
 * priority set, the threads past the run slots told to stop and the ones
 * let in woken only once they have, first sleeps, a change another
 * overtakes; and under ThreadSanitizer threads sleeping, waking each
 * other, yielding and setting priorities at once - never more threads on
 * their hardware threads than the run slots, never a thread let in before
 * the ones it replaces stopped, never a sleeping thread woken, each
 * thread's edges put once each and in order. C89. */

#include <stdlib.h>
#include <string.h>

#include "../../Emu/Cell/lv2/lv2_sched.h"
#include "test_common.h"

#include <rthreads/rthreads.h>
#include <sched.h>

static lv2_sched_t s;

/* The deterministic part: the edges put, in order */
static int edges_of[LV2_SCHED_THREADS][16];
static int edges_n[LV2_SCHED_THREADS];

/* A change made as an edge is being put, when armed: (slot, edge) */
static void (*in_put_hook)(unsigned slot, int edge);
static int in_put[LV2_SCHED_THREADS];
static int two_at_once;

static void record(void *ctx, unsigned slot, int edge)
{
   (void)ctx;
   if (in_put[slot]++)
      two_at_once++;
   if (edges_n[slot] < 16)
      edges_of[slot][edges_n[slot]] = edge;
   edges_n[slot]++;
   if (in_put_hook)
   {
      void (*hook)(unsigned, int) = in_put_hook;
      in_put_hook = NULL;
      hook(slot, edge);
   }
   in_put[slot]--;
}

static int last_edge(unsigned slot)
{
   return edges_n[slot] ? edges_of[slot][edges_n[slot] - 1] : -1;
}

static lv2_sched_op_t awake(int target, int prio, int caller)
{
   lv2_sched_op_t op;
   lv2_sched_op_init(&op, caller, 1);
   op.target = target;
   op.prio   = prio;
   lv2_sched_change(&s, &op, record, NULL);
   return op;
}

static lv2_sched_op_t sleep_(int t)
{
   lv2_sched_op_t op;
   lv2_sched_op_init(&op, t, 1);
   op.sleeper = t;
   lv2_sched_change(&s, &op, record, NULL);
   return op;
}

static int order_is(const char *want)
{
   lv2_sched_view_t v;
   char got[64];
   unsigned i;

   lv2_sched_read(&s, &v);
   for (i = 0; i < v.count && i < 63u; i++)
      got[i] = (char)('0' + v.list[i]);
   got[i] = 0;
   if (strcmp(got, want))
      fprintf(stderr, "order %s, wanted %s\n", got, want);
   return !strcmp(got, want);
}

/* A change made between another's read and its compare-and-swap, when
 * armed */
static int window_awake = -1;

void lv2_sched_test_window(lv2_sched_t *sc)
{
   (void)sc;
   if (window_awake >= 0)
   {
      const int t = window_awake;
      window_awake = -1;
      awake(t, LV2_SCHED_ENQUEUE, -1);
   }
}

/* As slot 1's run is put, it sleeps: its stop waits for the run's put */
static void sleep_one(unsigned slot, int edge)
{
   lv2_sched_op_t op;
   TEST_CHECK(slot == 1 && edge == LV2_SCHED_RUN);
   lv2_sched_op_init(&op, 1, 1);
   op.sleeper = 1;
   lv2_sched_change(&s, &op, record, NULL);
   TEST_CHECK(op.result == LV2_SCHED_SLEPT && edges_n[1] == 1);
}

/* As slot 2's stop is put, it makes a change of its own: not taken as
 * stopping, its stop not yet put - the one coming not let in */
static void told_changes(unsigned slot, int edge)
{
   lv2_sched_op_t op;
   TEST_CHECK(slot == 2 && edge == LV2_SCHED_STOP);
   lv2_sched_op_init(&op, 2, 1);
   op.target = 2;
   op.prio   = LV2_SCHED_ENQUEUE;
   lv2_sched_change(&s, &op, record, NULL);
   TEST_CHECK(edges_n[3] == 0);
}

static void puts_one_at_a_time(void)
{
   lv2_sched_op_t op;

   lv2_sched_init(&s, 2);
   memset(edges_n, 0, sizeof(edges_n));
   two_at_once = 0;

   /* An edge made as the one before it is put: put after, not in it */
   in_put_hook = sleep_one;
   awake(1, LV2_SCHED_ENQUEUE, -1);
   TEST_CHECK(!two_at_once && edges_n[1] == 2 && last_edge(1) == LV2_SCHED_STOP && order_is(""));

   /* A thread told to stop changes something as its stop is put */
   awake(1, 10, -1);
   awake(1, LV2_SCHED_ENQUEUE, -1);
   awake(2, 10, -1);
   awake(2, LV2_SCHED_ENQUEUE, -1);
   awake(3, 5, -1);
   in_put_hook = told_changes;
   awake(3, LV2_SCHED_ENQUEUE, -1);
   TEST_CHECK(order_is("312") && last_edge(2) == LV2_SCHED_STOP && edges_n[3] == 0);
   /* It sees its stop: then it is taken as stopping */
   lv2_sched_op_init(&op, 2, 1);
   op.target  = 2;
   op.prio    = LV2_SCHED_ENQUEUE;
   op.stopped = 1;
   lv2_sched_change(&s, &op, record, NULL);
   TEST_CHECK(last_edge(3) == LV2_SCHED_RUN && !two_at_once);
}

static void basics(void)
{
   lv2_sched_op_t op;

   lv2_sched_init(&s, 2);

   /* First sleeps: a PPU thread's marked, another's counted */
   lv2_sched_first_sleep(&s, 1);
   lv2_sched_first_sleep(&s, -1);
   TEST_CHECK(lv2_sched_first_sleeps(&s) == 2);
   op = sleep_(1);
   TEST_CHECK(op.result == LV2_SCHED_FIRST_SLEEP && !op.last_first_sleep);
   TEST_CHECK(lv2_sched_other_slept(&s) == 1 && lv2_sched_first_sleeps(&s) == 0);
   TEST_CHECK(!lv2_sched_other_slept(&s));
   TEST_CHECK(sleep_(1).result == LV2_SCHED_NOT_QUEUED);

   /* A priority set on a thread not queued; then it is queued and let run */
   op = awake(1, 10, -1);
   TEST_CHECK(op.result == 1 && op.prio_order == -1);
   op = awake(1, LV2_SCHED_ENQUEUE, -1);
   TEST_CHECK(op.changed && op.had_free_slot && op.lowest_new_prio == 10 && (op.emplaced[0] >> 1 & 1u));
   TEST_CHECK(last_edge(1) == LV2_SCHED_RUN && order_is("1"));
   awake(2, 10, -1);
   awake(2, LV2_SCHED_ENQUEUE, -1);
   TEST_CHECK(last_edge(2) == LV2_SCHED_RUN && order_is("12"));

   /* Queued again: found, nothing changed */
   op = awake(2, LV2_SCHED_ENQUEUE, -1);
   TEST_CHECK(!op.changed && (op.found[0] >> 2 & 1u) && edges_n[2] == 1);

   /* A better one comes: the one past the run slots told to stop, and the
    * new one not let in until it has */
   awake(3, 5, -1);
   op = awake(3, LV2_SCHED_ENQUEUE, -1);
   TEST_CHECK(op.changed && !op.had_free_slot && order_is("312"));
   TEST_CHECK(last_edge(2) == LV2_SCHED_STOP && edges_n[3] == 0);
   /* It stops (its awake of itself): the new one let in */
   op = awake(2, LV2_SCHED_ENQUEUE, 2);
   TEST_CHECK(!op.changed && last_edge(3) == LV2_SCHED_RUN && edges_n[2] == 2);

   /* A yield to the back of its equals, past the run slots: a switch */
   awake(4, 10, -1);
   awake(4, LV2_SCHED_ENQUEUE, -1);
   TEST_CHECK(order_is("3124") && edges_n[4] == 0);
   op = awake(1, LV2_SCHED_YIELD, 1);
   /* Told to stop in its own change, it is taken as stopping: the next
    * let in at once, as the turn did */
   TEST_CHECK(op.switched && op.changed && order_is("3241") && last_edge(1) == LV2_SCHED_STOP);
   TEST_CHECK(last_edge(2) == LV2_SCHED_RUN && lv2_sched_place(&s, 1) == 3);
   op = awake(1, LV2_SCHED_ENQUEUE, 1);
   TEST_CHECK(!op.changed && edges_n[1] == 2 && edges_n[2] == 3);

   /* A yield within the run slots rotates them, no switch */
   lv2_sched_init(&s, 3);
   memset(edges_n, 0, sizeof(edges_n));
   awake(5, LV2_SCHED_ENQUEUE, -1);
   awake(6, LV2_SCHED_ENQUEUE, -1);
   op = awake(5, LV2_SCHED_YIELD, 5);
   TEST_CHECK(!op.switched && !op.changed && order_is("65") && edges_n[5] == 1);
   op = awake(9, LV2_SCHED_YIELD, 9);
   TEST_CHECK(!op.changed);

   /* A thread setting its own same priority keeps its place; a queued
    * one set another goes to the back of its new equals */
   op = awake(6, 0, 6);
   TEST_CHECK(op.result && order_is("65"));
   awake(7, LV2_SCHED_ENQUEUE, -1);
   op = awake(6, 0, 5);
   TEST_CHECK(op.changed && order_is("576"));
   op = awake(5, 3, -1);
   TEST_CHECK(order_is("765") && lv2_sched_count(&s) == 3 && lv2_sched_at(&s, 0) == 7);

   /* Sleeping: out of the queue, its own edge */
   op = sleep_(6);
   TEST_CHECK(op.result == LV2_SCHED_SLEPT && last_edge(6) == LV2_SCHED_STOP && order_is("75"));
   op = sleep_(7);
   TEST_CHECK(op.result == LV2_SCHED_SLEPT && order_is("5"));
   {
      lv2_sched_op_t sig;
      lv2_sched_op_init(&sig, 5, 1);
      sig.sleeper  = 5;
      sig.signaled = 1;
      lv2_sched_change(&s, &sig, record, NULL);
      TEST_CHECK(sig.result == LV2_SCHED_NOT_SLEPT && order_is("5"));
   }

   /* Not ready yet: nothing let in */
   {
      lv2_sched_op_t nr;
      lv2_sched_op_init(&nr, -1, 0);
      nr.target = 8;
      nr.prio   = LV2_SCHED_ENQUEUE;
      lv2_sched_change(&s, &nr, record, NULL);
      TEST_CHECK(nr.changed && edges_n[8] == 0 && order_is("85"));
      awake(-1, LV2_SCHED_ENQUEUE, -1);
      TEST_CHECK(last_edge(8) == LV2_SCHED_RUN);
   }

   /* Another change between a change's read and its swap: both stand */
   window_awake = 9;
   op = awake(3, LV2_SCHED_ENQUEUE, -1);
   TEST_CHECK(op.changed && order_is("8935") && last_edge(5) == LV2_SCHED_STOP && last_edge(9) == LV2_SCHED_RUN);
   TEST_CHECK(edges_n[3] == 0 && edges_n[9] == 1);

   /* A sleep's threads to wake, in the same change */
   {
      unsigned char wake[2];
      lv2_sched_op_t so;
      wake[0] = 6; wake[1] = 5;
      lv2_sched_op_init(&so, 9, 1);
      so.sleeper        = 9;
      so.to_awake       = wake;
      so.to_awake_count = 2;
      lv2_sched_change(&s, &so, record, NULL);
      TEST_CHECK(so.result == LV2_SCHED_SLEPT && (so.emplaced[0] >> 6 & 1u) && (so.found[0] >> 5 & 1u));
      TEST_CHECK(order_is("8365") && last_edge(9) == LV2_SCHED_STOP && edges_n[6] == 2);
   }
}

/* The threads at once. Each model thread waits to be let in, then runs:
 * sleeps (to be woken by another, or the waker), yields, sets
 * priorities, wakes a sleeping one; when it sees it is told to stop it
 * stops, and says so. A thread in a change of its own is in the kernel,
 * off its hardware thread, as the turn took it to be; it goes back on
 * only when it sees it is let run. */
#define THREADS 12
#define SLOTS   3
#define ROUNDS  4000
/* Yields a thread waits before calling its wakeup lost */
#define STUCK   4000000L

static retro_atomic_int_t let_in[THREADS];   /* its last edge put: 1 let run */
static retro_atomic_int_t runs[THREADS];     /* its lets in put */
static retro_atomic_int_t last[THREADS];
static retro_atomic_int_t asleep[THREADS];   /* asleep, for another to wake */
static retro_atomic_int_t occupied;          /* on their hardware threads */
static retro_atomic_int_t errors;
static retro_atomic_int_t stuck;
static retro_atomic_int_t done_count;
static retro_atomic_int_t most;
static retro_atomic_int_t puts_count;

static void put(void *ctx, unsigned slot, int edge)
{
   (void)ctx;
   retro_atomic_inc_int(&puts_count);
   if (slot >= THREADS || retro_atomic_exchange_int(&last[slot], edge) == edge)
   {
      /* Not its next edge: two the same in a row */
      retro_atomic_inc_int(&errors);
      return;
   }
   if (edge == LV2_SCHED_RUN)
   {
      if (retro_atomic_load_acquire_int(&asleep[slot]))
         retro_atomic_inc_int(&errors); /* woken asleep */
      retro_atomic_store_release_int(&let_in[slot], 1);
      retro_atomic_inc_int(&runs[slot]);
   }
   else
      retro_atomic_store_release_int(&let_in[slot], 0);
}

static unsigned next_rand(unsigned *x)
{
   *x = *x * 1103515245u + 12345u;
   return (*x >> 16) & 0x7fffu;
}

static void on_hw(void)
{
   const int now = retro_atomic_fetch_add_int(&occupied, 1) + 1;
   if (now > SLOTS)
      retro_atomic_inc_int(&errors);
   if (now > retro_atomic_load_acquire_int(&most))
      retro_atomic_store_release_int(&most, now);
}

static void off_hw(void)
{
   retro_atomic_dec_int(&occupied);
}

/* Off its hardware thread (stopped, or in the kernel): back on once it
 * sees it is let run - 1 - or told to stop, which it says (its awake of
 * itself) and waits to be let in again. 0 when it waited too long */
static int go_back(unsigned me, int stopped, int seen_before)
{
   for (;;)
   {
      /* Stopped already: its lets in from before it stopped */
      const int seen = stopped ? seen_before : retro_atomic_load_acquire_int(&runs[me]);
      lv2_sched_op_t op;

      if (!stopped && retro_atomic_load_acquire_int(&let_in[me]))
      {
         on_hw();
         return 1;
      }

      if (!stopped)
      {
         lv2_sched_op_init(&op, (int)me, 1);
         op.target  = (int)me;
         op.prio    = LV2_SCHED_ENQUEUE;
         op.stopped = 1;
         lv2_sched_change(&s, &op, put, NULL);
         if (op.changed)
            retro_atomic_inc_int(&errors); /* it was in the queue */
      }
      stopped = 0;

      /* Let in again since it looked: it looks again */
      if (retro_atomic_load_acquire_int(&runs[me]) != seen)
         continue;

      {
         long spins = 0;
         while (retro_atomic_load_acquire_int(&runs[me]) == seen)
         {
            if (++spins > STUCK || retro_atomic_load_acquire_int(&stuck))
            {
               if (!retro_atomic_exchange_int(&stuck, 1))
                  retro_atomic_inc_int(&errors);
               return 0;
            }
            sched_yield();
         }
      }
   }
}

static void own_change(lv2_sched_op_t *op)
{
   off_hw();
   lv2_sched_change(&s, op, put, NULL);
}

static void wake_one(unsigned *x, int caller)
{
   const unsigned t = next_rand(x) % THREADS;
   if (retro_atomic_cas_int(&asleep[t], 1, 0))
   {
      lv2_sched_op_t op;
      lv2_sched_op_init(&op, caller, 1);
      op.target = (int)t;
      op.prio   = LV2_SCHED_ENQUEUE;
      lv2_sched_change(&s, &op, put, NULL);
   }
}

static void model(void *data)
{
   const unsigned me = (unsigned)(size_t)data;
   unsigned x = me * 7919u + 1u;
   unsigned r;

   /* Let in at the start, or waits to be */
   if (!go_back(me, !retro_atomic_load_acquire_int(&let_in[me]), 0))
      return;

   for (r = 0; r < ROUNDS;)
   {
      lv2_sched_op_t op;
      unsigned a;
      int slept = 0;
      int seen  = 0;

      if (!retro_atomic_load_acquire_int(&let_in[me]))
      {
         /* Told to stop, and it sees it */
         off_hw();
         if (!go_back(me, 0, 0))
            return;
         continue;
      }

      /* A round: something done */
      r++;
      a = next_rand(&x) % 8u;

      lv2_sched_op_init(&op, (int)me, 1);
      if (a < 2u || r == ROUNDS)
      {
         /* Sleeps; the last time for good */
         seen = retro_atomic_load_acquire_int(&runs[me]);
         op.sleeper = (int)me;
         own_change(&op);
         if (op.result != LV2_SCHED_SLEPT || retro_atomic_load_acquire_int(&runs[me]) != seen)
            retro_atomic_inc_int(&errors);
         if (r == ROUNDS)
            break;
         retro_atomic_store_release_int(&asleep[me], 1);
         slept = 1;
      }
      else if (a == 2u)
      {
         /* Yields */
         op.target = (int)me;
         op.prio   = LV2_SCHED_YIELD;
         own_change(&op);
      }
      else if (a == 3u)
      {
         /* A priority of its own */
         op.target = (int)me;
         op.prio   = (int)(next_rand(&x) % 4u);
         own_change(&op);
      }
      else if (a <= 5u)
      {
         off_hw();
         wake_one(&x, (int)me);
      }
      else
      {
         sched_yield();
         continue;
      }

      if (!go_back(me, slept, seen))
         return;
   }
   retro_atomic_inc_int(&done_count);
}

/* Each snapshot as it stands: the queue in order with no thread twice,
 * the ones let run in the run slots, the ones told to stop counted, and
 * with none of those the run slots all let run - the best threads */
static retro_atomic_int_t checks;

static int whole(const lv2_sched_view_t *v)
{
   unsigned i, queued = 0, told = 0;
   unsigned char in[LV2_SCHED_THREADS];

   memset(in, 0, sizeof(in));
   for (i = 0; i < v->count; i++)
   {
      const unsigned t = v->list[i];
      if (t >= LV2_SCHED_THREADS || in[t]++ || !LV2_SCHED_QUEUED(v->thread[t]))
         return 0;
      if (i && LV2_SCHED_PRIO(v->thread[v->list[i - 1]]) > LV2_SCHED_PRIO(v->thread[t]))
         return 0;
      if (i < SLOTS && !v->pending && !LV2_SCHED_LET_RUN(v->thread[t]))
         return 0;
   }
   for (i = 0; i < LV2_SCHED_THREADS; i++)
   {
      const uint32_t w = v->thread[i];
      queued += LV2_SCHED_QUEUED(w);
      told   += LV2_SCHED_TOLD(w);
      if (LV2_SCHED_TOLD(w) && LV2_SCHED_LET_RUN(w))
         return 0;
      if (LV2_SCHED_LET_RUN(w))
      {
         unsigned at;
         for (at = 0; at < v->count && v->list[at] != i; at++)
            ;
         if (at >= v->count || at >= SLOTS)
            return 0;
      }
   }
   return queued == v->count && told == v->pending;
}

static void checker(void *data)
{
   (void)data;
   while (retro_atomic_load_acquire_int(&done_count) < THREADS && !retro_atomic_load_acquire_int(&stuck))
   {
      lv2_sched_view_t v;
      lv2_sched_read(&s, &v);
      if (!whole(&v))
         retro_atomic_inc_int(&errors);
      retro_atomic_inc_int(&checks);
      sched_yield();
   }
}

/* An SPU or RSX thread, waking sleeping ones */
static void waker(void *data)
{
   unsigned x = 4242u;
   (void)data;
   while (retro_atomic_load_acquire_int(&done_count) < THREADS && !retro_atomic_load_acquire_int(&stuck))
   {
      wake_one(&x, -1);
      sched_yield();
   }
}

static void threads(void)
{
   sthread_t *t[THREADS];
   sthread_t *w;
   sthread_t *c;
   lv2_sched_view_t v;
   unsigned i;

   lv2_sched_init(&s, SLOTS);
   retro_atomic_int_init(&occupied, 0);
   retro_atomic_int_init(&errors, 0);
   retro_atomic_int_init(&stuck, 0);
   retro_atomic_int_init(&done_count, 0);
   retro_atomic_int_init(&most, 0);
   retro_atomic_int_init(&puts_count, 0);
   retro_atomic_int_init(&checks, 0);
   for (i = 0; i < THREADS; i++)
   {
      retro_atomic_int_init(&let_in[i], 0);
      retro_atomic_int_init(&last[i], LV2_SCHED_STOP);
      retro_atomic_int_init(&runs[i], 0);
      retro_atomic_int_init(&asleep[i], 0);
   }

   /* All queued at the start, the first let in */
   for (i = 0; i < THREADS; i++)
   {
      lv2_sched_op_t op;
      lv2_sched_op_init(&op, -1, 1);
      op.target = (int)i;
      op.prio   = LV2_SCHED_ENQUEUE;
      lv2_sched_change(&s, &op, put, NULL);
   }

   for (i = 0; i < THREADS; i++)
      t[i] = sthread_create(model, (void*)(size_t)i);
   w = sthread_create(waker, NULL);
   c = sthread_create(checker, NULL);
   for (i = 0; i < THREADS; i++)
      sthread_join(t[i]);
   sthread_join(w);
   sthread_join(c);

   lv2_sched_read(&s, &v);
   if (retro_atomic_load_acquire_int(&stuck))
   {
      fprintf(stderr, "stuck: count %u pending %u:", v.count, v.pending);
      for (i = 0; i < v.count; i++)
         fprintf(stderr, " %u", v.list[i]);
      fprintf(stderr, "\n");
      for (i = 0; i < THREADS; i++)
         fprintf(stderr, " %u: w %08x let_in %d asleep %d applied %d\n", i, v.thread[i],
               retro_atomic_load_acquire_int(&let_in[i]),
               retro_atomic_load_acquire_int(&asleep[i]), retro_atomic_load_acquire_int(&s.applied[i]));
   }
   TEST_CHECK(!retro_atomic_load_acquire_int(&stuck));
   TEST_CHECK(retro_atomic_load_acquire_int(&errors) == 0);
   TEST_CHECK(v.count == 0 && v.pending == 0 && retro_atomic_load_acquire_int(&occupied) == 0);
   for (i = 0; i < THREADS; i++)
      TEST_CHECK(!LV2_SCHED_LET_RUN(v.thread[i]) && !retro_atomic_load_acquire_int(&let_in[i]));
   TEST_CHECK(whole(&v) && retro_atomic_load_acquire_int(&checks) > 0);
   printf("test_lv2_sched: %d threads, %d run slots: %d edges put, at most %d on their hardware threads, %d snapshots whole\n",
         THREADS, SLOTS, retro_atomic_load_acquire_int(&puts_count), retro_atomic_load_acquire_int(&most),
         retro_atomic_load_acquire_int(&checks));
}

int main(void)
{
   basics();
   puts_one_at_a_time();
   threads();
   if (test_failures)
   {
      fprintf(stderr, "test_lv2_sched: %d failed\n", test_failures);
      return 1;
   }
   printf("test_lv2_sched: in order, yields, priorities, stops before starts, first sleeps, a change overtaken\n");
   return 0;
}
