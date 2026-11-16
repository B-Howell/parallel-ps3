/* The lock-free pieces the lv2 sync objects are built on: the set of
 * waiting threads (lv2_waitset.c) and an event queue's ring
 * (lv2_event_ring.c). In order and by priority as lv2 wakes; under
 * ThreadSanitizer, threads joining, leaving and being woken at once with
 * every wait ended exactly once, and senders and receivers at once with
 * every event received exactly once and each sender's in order. C89. */

#include <stdlib.h>
#include <string.h>

#include "../../Emu/Cell/lv2/lv2_waitset.h"
#include "../../Emu/Cell/lv2/lv2_event_ring.h"
#include "test_common.h"

#include <rthreads/rthreads.h>
#include <sched.h>

static int prio[LV2_WAITSET_SLOTS];

static int by_prio(void *ctx, unsigned slot)
{
   (void)ctx;
   return prio[slot];
}

static void waitset_order(void)
{
   lv2_waitset_t ws;
   retro_u128_t all;

   lv2_waitset_init(&ws);
   TEST_CHECK(lv2_waitset_claim(&ws, NULL, NULL) == -1);
   TEST_CHECK(lv2_waitset_join(&ws, 5) && lv2_waitset_join(&ws, 3) && lv2_waitset_join(&ws, 100));
   TEST_CHECK(!lv2_waitset_join(&ws, 5));
   TEST_CHECK(!lv2_waitset_join(&ws, LV2_WAITSET_SLOTS));

   /* In the order they came */
   TEST_CHECK(lv2_waitset_claim(&ws, NULL, NULL) == 5);
   TEST_CHECK(lv2_waitset_claim(&ws, NULL, NULL) == 3);
   /* A claimed one cannot leave: it was woken */
   TEST_CHECK(!lv2_waitset_leave(&ws, 3));
   TEST_CHECK(lv2_waitset_leave(&ws, 100));
   TEST_CHECK(!lv2_waitset_any(&ws));

   /* By priority, then in the order they came */
   prio[7] = 2; prio[8] = 1; prio[9] = 1; prio[70] = 0;
   TEST_CHECK(lv2_waitset_join(&ws, 7) && lv2_waitset_join(&ws, 9) && lv2_waitset_join(&ws, 8) && lv2_waitset_join(&ws, 70));
   TEST_CHECK(lv2_waitset_claim(&ws, by_prio, NULL) == 70);
   TEST_CHECK(lv2_waitset_claim(&ws, by_prio, NULL) == 9);
   TEST_CHECK(lv2_waitset_claim(&ws, by_prio, NULL) == 8);
   TEST_CHECK(lv2_waitset_has(&ws, 7));

   /* All at once */
   TEST_CHECK(lv2_waitset_join(&ws, 127));
   all = lv2_waitset_claim_all(&ws);
   TEST_CHECK(all.lo == ((uint64_t)1 << 7) && all.hi == ((uint64_t)1 << 63));
   TEST_CHECK(!lv2_waitset_any(&ws));
}

static void ring_basics(void)
{
   lv2_event_ring_t r;
   uint64_t ev[4];
   int i;

   TEST_CHECK(lv2_event_ring_init(&r, 3));
   TEST_CHECK(!lv2_event_ring_pop(&r, ev));
   for (i = 0; i < 3; i++)
   {
      ev[0] = (uint64_t)i; ev[1] = 10u + (uint64_t)i; ev[2] = 0; ev[3] = 0;
      TEST_CHECK(lv2_event_ring_push(&r, ev));
   }
   /* Its size, though the ring has room for four */
   TEST_CHECK(!lv2_event_ring_push(&r, ev));
   TEST_CHECK(lv2_event_ring_count(&r) == 3);
   for (i = 0; i < 3; i++)
      TEST_CHECK(lv2_event_ring_pop(&r, ev) && ev[0] == (uint64_t)i && ev[1] == 10u + (uint64_t)i);
   TEST_CHECK(!lv2_event_ring_pop(&r, ev) && lv2_event_ring_count(&r) == 0);
   /* Around the ring many times */
   for (i = 0; i < 1000; i++)
   {
      ev[0] = (uint64_t)i;
      TEST_CHECK(lv2_event_ring_push(&r, ev));
      TEST_CHECK(lv2_event_ring_pop(&r, ev) && ev[0] == (uint64_t)i);
   }
   lv2_event_ring_deinit(&r);
}

/* Threads: waiters joining and leaving or being woken, wakers claiming */
#define WAITERS 8
#define ROUNDS 20000

static lv2_waitset_t shared_ws;
static retro_atomic_int_t given[WAITERS];
static retro_atomic_int_t woken_total;
static retro_atomic_int_t claimed_total;
static retro_atomic_int_t errors;
static retro_atomic_int_t stop;

static void waiter(void *data)
{
   const unsigned slot = (unsigned)(size_t)data;
   unsigned r;

   for (r = 0; r < ROUNDS; r++)
   {
      if (!lv2_waitset_join(&shared_ws, slot))
         retro_atomic_inc_int(&errors);
      if (r & 1)
         sched_yield();

      if (lv2_waitset_leave(&shared_ws, slot))
         continue; /* timed out */

      /* Claimed: it is given what it was woken for */
      while (!retro_atomic_load_acquire_int(&given[slot]))
         sched_yield();
      retro_atomic_store_release_int(&given[slot], 0);
      retro_atomic_inc_int(&woken_total);
   }
}

static void waker(void *data)
{
   (void)data;
   while (!retro_atomic_load_acquire_int(&stop))
   {
      const int s = lv2_waitset_claim(&shared_ws, NULL, NULL);
      if (s < 0)
      {
         sched_yield();
         continue;
      }
      if (retro_atomic_exchange_int(&given[s], 1) != 0)
         retro_atomic_inc_int(&errors);
      retro_atomic_inc_int(&claimed_total);
   }
}

static void waitset_threads(void)
{
   sthread_t *w[WAITERS], *k[2];
   unsigned i;

   lv2_waitset_init(&shared_ws);
   for (i = 0; i < WAITERS; i++)
      retro_atomic_int_init(&given[i], 0);
   retro_atomic_int_init(&stop, 0);

   for (i = 0; i < 2; i++)
      k[i] = sthread_create(waker, NULL);
   for (i = 0; i < WAITERS; i++)
      w[i] = sthread_create(waiter, (void*)(size_t)(i * 15u)); /* slots across both halves */
   for (i = 0; i < WAITERS; i++)
      sthread_join(w[i]);
   retro_atomic_store_release_int(&stop, 1);
   for (i = 0; i < 2; i++)
      sthread_join(k[i]);

   TEST_CHECK(retro_atomic_load_acquire_int(&errors) == 0);
   TEST_CHECK(retro_atomic_load_acquire_int(&woken_total) == retro_atomic_load_acquire_int(&claimed_total));
   TEST_CHECK(!lv2_waitset_any(&shared_ws));
   printf("test_lv2_waitset: %d waiters, 2 wakers: %d woken, the rest left, every wait ended once\n",
         WAITERS, retro_atomic_load_acquire_int(&woken_total));
}

/* Threads: senders and receivers on one ring */
#define SENDERS 4
#define SENT 50000

static lv2_event_ring_t shared_ring;
static retro_atomic_int_t received[SENDERS];
static retro_atomic_int_t senders_left;

static void sender(void *data)
{
   const uint64_t id = (uint64_t)(size_t)data;
   uint64_t ev[4];
   uint64_t n;

   for (n = 1; n <= SENT; n++)
   {
      ev[0] = id; ev[1] = n; ev[2] = ~n; ev[3] = id ^ n;
      while (!lv2_event_ring_push(&shared_ring, ev))
         sched_yield(); /* full: the test's own retry */
   }
   retro_atomic_dec_int(&senders_left);
}

static void receiver(void *data)
{
   uint64_t last[SENDERS];
   uint64_t ev[4];
   (void)data;
   memset(last, 0, sizeof(last));

   for (;;)
   {
      if (!lv2_event_ring_pop(&shared_ring, ev))
      {
         if (!retro_atomic_load_acquire_int(&senders_left) && !lv2_event_ring_count(&shared_ring))
            break;
         sched_yield();
         continue;
      }
      /* Whole, and each sender's in order */
      if (ev[0] >= SENDERS || ev[2] != ~ev[1] || ev[3] != (ev[0] ^ ev[1]) || ev[1] <= last[ev[0]])
         retro_atomic_inc_int(&errors);
      else
         last[ev[0]] = ev[1];
      retro_atomic_inc_int(&received[ev[0]]);
   }
}

static void ring_threads(void)
{
   sthread_t *s[SENDERS], *r[SENDERS];
   unsigned i;
   int ok = 1;

   TEST_CHECK(lv2_event_ring_init(&shared_ring, 127));
   retro_atomic_int_init(&senders_left, SENDERS);
   for (i = 0; i < SENDERS; i++)
      retro_atomic_int_init(&received[i], 0);
   for (i = 0; i < SENDERS; i++)
      r[i] = sthread_create(receiver, NULL);
   for (i = 0; i < SENDERS; i++)
      s[i] = sthread_create(sender, (void*)(size_t)i);
   for (i = 0; i < SENDERS; i++)
      sthread_join(s[i]);
   for (i = 0; i < SENDERS; i++)
      sthread_join(r[i]);

   for (i = 0; i < SENDERS; i++)
      ok &= retro_atomic_load_acquire_int(&received[i]) == SENT;
   TEST_CHECK(ok);
   TEST_CHECK(retro_atomic_load_acquire_int(&errors) == 0);
   lv2_event_ring_deinit(&shared_ring);
   printf("test_lv2_waitset: %d senders and receivers on a ring of 127: %d events, each received once, each sender's in order\n",
         SENDERS, SENDERS * SENT);
}

int main(void)
{
   retro_atomic_int_init(&errors, 0);
   waitset_order();
   ring_basics();
   waitset_threads();
   ring_threads();

   if (test_failures)
   {
      fprintf(stderr, "test_lv2_waitset: %d failed\n", test_failures);
      return 1;
   }
   printf("test_lv2_waitset: waiters woken in order and by priority, events in order at the queue's size\n");
   return 0;
}
