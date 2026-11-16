/* An lv2 mutex's owner and waiters in one word (lv2_ownq.c): taken, queued
 * in order and by priority, handed to the best waiter at unlock, given to
 * a condition's thread; and under ThreadSanitizer threads locking with
 * timeouts and unlocking at once - never two owners, every thread queued
 * either handed the mutex or out of the queue, none both. C89. */

#include <stdlib.h>
#include <string.h>

#include "../../Emu/Cell/lv2/lv2_ownq.h"
#include "test_common.h"

#include <rthreads/rthreads.h>
#include <sched.h>

static int prio[LV2_OWNQ_SLOTS];

static int by_prio(void *ctx, unsigned slot)
{
   (void)ctx;
   return prio[slot];
}

static void basics(void)
{
   lv2_ownq_t q;

   lv2_ownq_init(&q, -1);
   TEST_CHECK(lv2_ownq_owner(&q) == -1);
   TEST_CHECK(lv2_ownq_trylock(&q, 4) == LV2_OWNQ_OWNED && lv2_ownq_owner(&q) == 4);
   TEST_CHECK(lv2_ownq_trylock(&q, 4) == LV2_OWNQ_MINE && lv2_ownq_trylock(&q, 5) == LV2_OWNQ_BUSY);
   TEST_CHECK(lv2_ownq_lock(&q, 4) == LV2_OWNQ_MINE);
   TEST_CHECK(lv2_ownq_unlock(&q, 5, NULL, NULL) == LV2_OWNQ_NOT_OWNER);

   /* Queued in the order they came, handed over in it */
   TEST_CHECK(lv2_ownq_lock(&q, 70) == LV2_OWNQ_QUEUED && lv2_ownq_lock(&q, 9) == LV2_OWNQ_QUEUED);
   TEST_CHECK(lv2_ownq_lock(&q, 99) == LV2_OWNQ_QUEUED && lv2_ownq_lock(&q, 99) == LV2_OWNQ_BAD);
   TEST_CHECK(lv2_ownq_lock(&q, 100) == LV2_OWNQ_BAD);
   TEST_CHECK(lv2_ownq_unlock(&q, 4, NULL, NULL) == 70 && lv2_ownq_owner(&q) == 70);
   TEST_CHECK(!lv2_ownq_waits(&q, 70) && !lv2_ownq_leave(&q, 70));
   /* One leaves on its timeout; the other is handed it */
   TEST_CHECK(lv2_ownq_leave(&q, 9));
   TEST_CHECK(lv2_ownq_unlock(&q, 70, NULL, NULL) == 99);
   TEST_CHECK(lv2_ownq_unlock(&q, 99, NULL, NULL) == LV2_OWNQ_FREED && lv2_ownq_owner(&q) == -1);

   /* A condition's thread: owner when free, else queued - its own owner too */
   TEST_CHECK(lv2_ownq_give(&q, 3) == LV2_OWNQ_OWNED);
   TEST_CHECK(lv2_ownq_give(&q, 3) == LV2_OWNQ_QUEUED && lv2_ownq_waits(&q, 3));
   TEST_CHECK(lv2_ownq_unlock(&q, 3, NULL, NULL) == 3 && lv2_ownq_owner(&q) == 3);
   TEST_CHECK(!lv2_ownq_any_waiter(&q));

   /* By priority, then in the order they came */
   prio[10] = 2; prio[11] = 1; prio[12] = 1; prio[80] = 0;
   TEST_CHECK(lv2_ownq_lock(&q, 10) == LV2_OWNQ_QUEUED && lv2_ownq_lock(&q, 12) == LV2_OWNQ_QUEUED);
   TEST_CHECK(lv2_ownq_lock(&q, 11) == LV2_OWNQ_QUEUED && lv2_ownq_lock(&q, 80) == LV2_OWNQ_QUEUED);
   TEST_CHECK(lv2_ownq_unlock(&q, 3, by_prio, NULL) == 80);
   TEST_CHECK(lv2_ownq_unlock(&q, 80, by_prio, NULL) == 12);
   TEST_CHECK(lv2_ownq_unlock(&q, 12, by_prio, NULL) == 11);
   TEST_CHECK(lv2_ownq_unlock(&q, 11, by_prio, NULL) == 10);

   /* A save state's owner */
   lv2_ownq_init(&q, 42);
   TEST_CHECK(lv2_ownq_owner(&q) == 42);
}

#define THREADS 6
#define ROUNDS 20000

static lv2_ownq_t shared;
static retro_atomic_int_t handed[THREADS];
static retro_atomic_int_t inside;
static retro_atomic_int_t errors;
static unsigned long work; /* the owner's alone: plain memory */

static void hand(int to)
{
   if (to >= 0 && retro_atomic_exchange_int(&handed[to / 16], 1) != 0)
      retro_atomic_inc_int(&errors);
}

static void locker(void *data)
{
   const unsigned slot = (unsigned)(size_t)data;
   const unsigned me   = slot / 16u;
   unsigned r;

   for (r = 0; r < ROUNDS; r++)
   {
      const int got = lv2_ownq_lock(&shared, slot);

      if (got == LV2_OWNQ_QUEUED)
      {
         if (r % 3 == 0 && lv2_ownq_leave(&shared, slot))
            continue; /* timed out, never the owner */

         /* Handed it */
         while (!retro_atomic_load_acquire_int(&handed[me]))
            sched_yield();
         retro_atomic_store_release_int(&handed[me], 0);
      }
      else if (got != LV2_OWNQ_OWNED)
         retro_atomic_inc_int(&errors);

      if (lv2_ownq_owner(&shared) != (int)slot || retro_atomic_fetch_add_int(&inside, 1) != 0)
         retro_atomic_inc_int(&errors);
      work++;
      retro_atomic_dec_int(&inside);

      hand(lv2_ownq_unlock(&shared, slot, NULL, NULL));
   }
}

static void threads(void)
{
   sthread_t *t[THREADS];
   unsigned i;

   lv2_ownq_init(&shared, -1);
   retro_atomic_int_init(&inside, 0);
   retro_atomic_int_init(&errors, 0);
   for (i = 0; i < THREADS; i++)
      retro_atomic_int_init(&handed[i], 0);

   for (i = 0; i < THREADS; i++)
      t[i] = sthread_create(locker, (void*)(size_t)(i * 16u + 3u));
   for (i = 0; i < THREADS; i++)
      sthread_join(t[i]);

   TEST_CHECK(retro_atomic_load_acquire_int(&errors) == 0);
   TEST_CHECK(lv2_ownq_owner(&shared) == -1 && !lv2_ownq_any_waiter(&shared));
   printf("test_lv2_ownq: %d threads locking with timeouts: %lu times owned, never by two\n", THREADS, work);
}

int main(void)
{
   basics();
   threads();
   if (test_failures)
   {
      fprintf(stderr, "test_lv2_ownq: %d failed\n", test_failures);
      return 1;
   }
   printf("test_lv2_ownq: taken, queued in order and by priority, handed over at unlock, given to a condition's thread\n");
   return 0;
}
