/* An lv2 event queue's events and waiters with no lock (lv2_evq.c): taken
 * at once or waited for, a send handing the oldest event to the best
 * waiter, waiters in two words, a send that comes as a thread joins, time-
 * outs, a destroy's claim; and under ThreadSanitizer senders and receivers
 * at once, some timing out - every event sent taken exactly once, none
 * lost. C89. */

#include <stdlib.h>
#include <string.h>

#include "../../Emu/Cell/lv2/lv2_evq.h"
#include "test_common.h"

#include <rthreads/rthreads.h>
#include <sched.h>

static int prio[LV2_EVQ_SLOTS];

static int by_prio(void *ctx, unsigned slot)
{
   (void)ctx;
   return prio[slot];
}

static int has(const retro_u128_t w[2], unsigned slot)
{
   const retro_u128_t *x = &w[slot / 128u];
   const unsigned s      = slot % 128u;
   return s < 64u ? (int)((x->lo >> s) & 1u) : (int)((x->hi >> (s - 64u)) & 1u);
}

static lv2_evq_t q;
static uint64_t window_send; /* sent between a waiter's look and its join, when armed */

void lv2_evq_test_window(lv2_evq_t *eq)
{
   if (window_send)
   {
      uint64_t ev[4];
      unsigned slot = 0;
      ev[0] = window_send; ev[1] = ev[2] = ev[3] = 0;
      window_send = 0;
      TEST_CHECK(lv2_evq_send(eq, ev, NULL, NULL, &slot) == LV2_EVQ_SENT);
   }
}

static void basics(void)
{
   uint64_t ev[4] = {0, 0, 0, 0};
   uint64_t got[4];
   unsigned slot = 999;
   retro_u128_t woken[2];
   int i;

   TEST_CHECK(lv2_evq_init(&q, 3));
   TEST_CHECK(!lv2_evq_try(&q, got));

   /* Held up to its size, taken in order */
   for (i = 1; i <= 3; i++)
   {
      ev[0] = (uint64_t)i;
      TEST_CHECK(lv2_evq_send(&q, ev, NULL, NULL, &slot) == LV2_EVQ_SENT);
   }
   ev[0] = 4;
   TEST_CHECK(lv2_evq_send(&q, ev, NULL, NULL, &slot) == LV2_EVQ_FULL && lv2_evq_count(&q) == 3);
   TEST_CHECK(lv2_evq_wait(&q, 5, got) == LV2_EVQ_GOT && got[0] == 1);
   TEST_CHECK(lv2_evq_try(&q, got) && got[0] == 2 && lv2_evq_try(&q, got) && got[0] == 3);

   /* Waiters in both words; the best given the event */
   prio[5] = 3; prio[200] = 1; prio[130] = 1;
   TEST_CHECK(lv2_evq_wait(&q, 5, got) == LV2_EVQ_WAITING && lv2_evq_wait(&q, 200, got) == LV2_EVQ_WAITING);
   TEST_CHECK(lv2_evq_wait(&q, 130, got) == LV2_EVQ_WAITING && lv2_evq_wait(&q, 130, got) == LV2_EVQ_BAD);
   TEST_CHECK(lv2_evq_wait(&q, 256, got) == LV2_EVQ_BAD && lv2_evq_busy(&q));
   ev[0] = 10;
   TEST_CHECK(lv2_evq_send(&q, ev, by_prio, NULL, &slot) == LV2_EVQ_HANDED && slot == 200 && ev[0] == 10);
   lv2_evq_give(&q, slot, LV2_EVQ_TAKEN);
   TEST_CHECK(lv2_evq_verdict(&q, 200) == LV2_EVQ_TAKEN && lv2_evq_count(&q) == 0);
   ev[0] = 11;
   TEST_CHECK(lv2_evq_send(&q, ev, by_prio, NULL, &slot) == LV2_EVQ_HANDED && slot == 130);
   lv2_evq_give(&q, slot, LV2_EVQ_TAKEN);

   /* One times out; a destroy claims the rest */
   TEST_CHECK(lv2_evq_wait(&q, 7, got) == LV2_EVQ_WAITING);
   TEST_CHECK(lv2_evq_timeout(&q, 7) == LV2_EVQ_TIMEDOUT && !lv2_evq_waits(&q, 7));
   TEST_CHECK(lv2_evq_claim_all(&q, woken) == 1 && has(woken, 5));
   lv2_evq_give(&q, 5, LV2_EVQ_CANCELED);
   TEST_CHECK(lv2_evq_verdict(&q, 5) == LV2_EVQ_CANCELED && !lv2_evq_busy(&q));

   /* Given an event before its time ran out: that stands */
   TEST_CHECK(lv2_evq_wait(&q, 9, got) == LV2_EVQ_WAITING);
   ev[0] = 12;
   TEST_CHECK(lv2_evq_send(&q, ev, NULL, NULL, &slot) == LV2_EVQ_HANDED && slot == 9);
   lv2_evq_give(&q, slot, LV2_EVQ_TAKEN);
   TEST_CHECK(lv2_evq_timeout(&q, 9) == LV2_EVQ_TAKEN);

   /* A send as a thread joins, seeing no waiter: the thread sees the event
    * after it joined, and takes it */
   window_send = 77;
   TEST_CHECK(lv2_evq_wait(&q, 3, got) == LV2_EVQ_GOT && got[0] == 77 && !lv2_evq_any_waiter(&q));

   /* A sender watching for room is told when the queue is gone: a poke
    * moves the count it waits on, and a wait on the old count returns */
   {
      const unsigned seen = lv2_evq_watch(&q);
      int moved;
      lv2_evq_poke(&q);
      moved = (unsigned)retro_atomic_load_acquire_int(&q.changes) != seen;
      TEST_CHECK(moved);
      if (moved)
         lv2_evq_watch_wait(&q, seen, -1);
      lv2_evq_unwatch(&q);
   }
   lv2_evq_deinit(&q);
}

#define RECEIVERS 4
#define SENDERS 2
#define PER_SENDER 20000
/* Yields a receiver waits before calling its wakeup lost */
#define STUCK 2000000L

static retro_atomic_int_t seen[SENDERS * PER_SENDER];
static retro_atomic_int_t errors;
static retro_atomic_int_t received;
static retro_atomic_int_t stop;
static retro_atomic_int_t stuck;
/* What a send handed to a waiter, there for it to read */
static uint64_t handed[LV2_EVQ_SLOTS];

static void take(uint64_t v)
{
   if (v >= SENDERS * PER_SENDER || retro_atomic_exchange_int(&seen[v], 1) != 0)
      retro_atomic_inc_int(&errors);
   retro_atomic_inc_int(&received);
}

static void receiver(void *data)
{
   const unsigned slot = (unsigned)(size_t)data * 61u + 7u;
   unsigned r = 0;

   while (!retro_atomic_load_acquire_int(&stop) && !retro_atomic_load_acquire_int(&stuck))
   {
      uint64_t got[4];
      const int w = lv2_evq_wait(&q, slot, got);
      int v;

      r++;
      if (w == LV2_EVQ_GOT)
      {
         take(got[0]);
         continue;
      }
      if (w != LV2_EVQ_WAITING)
      {
         retro_atomic_inc_int(&errors);
         return;
      }

      /* Its time runs out before a send takes it, or after: then its
       * verdict is waited for, as below */
      if (r % 3 == 0 && (sched_yield(), lv2_evq_leave(&q, slot)))
         v = LV2_EVQ_TIMEDOUT;
      else
      {
         long spins = 0;
         while ((v = lv2_evq_verdict(&q, slot)) == LV2_EVQ_PENDING)
         {
            if (retro_atomic_load_acquire_int(&stop) && lv2_evq_leave(&q, slot))
            {
               v = LV2_EVQ_TIMEDOUT;
               break;
            }
            if (++spins > STUCK)
            {
               retro_atomic_store_release_int(&stuck, 1);
               retro_atomic_inc_int(&errors);
               return;
            }
            sched_yield();
         }
      }

      if (v == LV2_EVQ_TAKEN)
         take(handed[slot]);
   }
}

static void sender(void *data)
{
   const unsigned me = (unsigned)(size_t)data;
   unsigned i;

   for (i = 0; i < PER_SENDER; i++)
   {
      uint64_t ev[4];
      unsigned slot = 0;
      int sent;

      ev[0] = (uint64_t)(me * PER_SENDER + i); ev[1] = ev[2] = ev[3] = 0;
      while ((sent = lv2_evq_send(&q, ev, NULL, NULL, &slot)) == LV2_EVQ_FULL)
      {
         if (retro_atomic_load_acquire_int(&stuck))
            return;
         sched_yield();
      }
      if (sent == LV2_EVQ_HANDED)
      {
         handed[slot] = ev[0];
         lv2_evq_give(&q, slot, LV2_EVQ_TAKEN);
      }
   }
}

static void threads(void)
{
   sthread_t *rt[RECEIVERS];
   sthread_t *st[SENDERS];
   uint64_t got[4];
   unsigned i;

   TEST_CHECK(lv2_evq_init(&q, 8));
   for (i = 0; i < SENDERS * PER_SENDER; i++)
      retro_atomic_int_init(&seen[i], 0);
   retro_atomic_int_init(&errors, 0);
   retro_atomic_int_init(&received, 0);
   retro_atomic_int_init(&stop, 0);
   retro_atomic_int_init(&stuck, 0);

   for (i = 0; i < RECEIVERS; i++)
      rt[i] = sthread_create(receiver, (void*)(size_t)i);
   for (i = 0; i < SENDERS; i++)
      st[i] = sthread_create(sender, (void*)(size_t)i);
   for (i = 0; i < SENDERS; i++)
      sthread_join(st[i]);
   retro_atomic_store_release_int(&stop, 1);
   for (i = 0; i < RECEIVERS; i++)
      sthread_join(rt[i]);

   /* What is left in the queue, taken here */
   while (lv2_evq_try(&q, got))
      take(got[0]);
   TEST_CHECK(!retro_atomic_load_acquire_int(&stuck));

   TEST_CHECK(retro_atomic_load_acquire_int(&errors) == 0);
   TEST_CHECK(retro_atomic_load_acquire_int(&received) == SENDERS * PER_SENDER);
   TEST_CHECK(!lv2_evq_busy(&q));
   printf("test_lv2_evq: %d senders, %d receivers timing out: %d events, each taken once\n",
         SENDERS, RECEIVERS, retro_atomic_load_acquire_int(&received));
   lv2_evq_deinit(&q);
}

int main(void)
{
   basics();
   threads();
   if (test_failures)
   {
      fprintf(stderr, "test_lv2_evq: %d failed\n", test_failures);
      return 1;
   }
   printf("test_lv2_evq: taken or waited for, handed to the best waiter, time-outs, a send as a thread joins\n");
   return 0;
}
