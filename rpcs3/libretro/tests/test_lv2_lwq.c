/* An lv2 lightweight mutex's signal and sleepers in one word (lv2_lwq.c):
 * the signal taken or the thread asleep, handed to the best sleeper or
 * left as the signal, an unlock2's signal and sleepers told so; and under
 * ThreadSanitizer threads locking with timeouts and unlocking at once -
 * never two owners, every sleeper either given it or out of the queue.
 * C89. */

#include <stdlib.h>
#include <string.h>

#include "../../Emu/Cell/lv2/lv2_lwq.h"
#include "test_common.h"

#include <rthreads/rthreads.h>
#include <sched.h>

static int prio[LV2_LWQ_SLOTS];

static int by_prio(void *ctx, unsigned slot)
{
   (void)ctx;
   return prio[slot];
}

static void basics(void)
{
   lv2_lwq_t q;
   uint32_t sig = 0;

   lv2_lwq_init(&q, 0);
   TEST_CHECK(!lv2_lwq_take(&q) && !lv2_lwq_trylock(&q));

   /* Nobody sleeps: the unlock leaves the signal, the lock takes it */
   TEST_CHECK(lv2_lwq_unlock(&q, 0, NULL, NULL) == -1 && lv2_lwq_signal(&q) == LV2_LWQ_SIGNAL);
   TEST_CHECK(lv2_lwq_lock(&q, 4, &sig) == LV2_LWQ_OWNED && sig == LV2_LWQ_SIGNAL && !lv2_lwq_signal(&q));

   /* An unlock2's signal: taken by a lock, not by a trylock */
   TEST_CHECK(lv2_lwq_unlock_free(&q, 1) && lv2_lwq_signal(&q) == LV2_LWQ_SIGNAL2);
   TEST_CHECK(!lv2_lwq_trylock(&q) && lv2_lwq_take(&q) == LV2_LWQ_SIGNAL2);

   /* Sleepers in the order they came; one times out */
   TEST_CHECK(lv2_lwq_lock(&q, 70, &sig) == LV2_LWQ_QUEUED && lv2_lwq_lock(&q, 9, &sig) == LV2_LWQ_QUEUED);
   TEST_CHECK(lv2_lwq_lock(&q, 99, &sig) == LV2_LWQ_QUEUED && lv2_lwq_lock(&q, 99, &sig) == LV2_LWQ_BAD);
   TEST_CHECK(lv2_lwq_lock(&q, 100, &sig) == LV2_LWQ_BAD && !lv2_lwq_unlock_free(&q, 0));
   TEST_CHECK(lv2_lwq_unlock(&q, 0, NULL, NULL) == 70 && !lv2_lwq_given2(&q, 70) && !lv2_lwq_leave(&q, 70));
   TEST_CHECK(lv2_lwq_leave(&q, 9) && lv2_lwq_waits(&q, 99));
   TEST_CHECK(lv2_lwq_unlock(&q, 1, NULL, NULL) == 99 && lv2_lwq_given2(&q, 99) && !lv2_lwq_any_waiter(&q));
   TEST_CHECK(!lv2_lwq_signal(&q));

   /* By priority, then in the order they came */
   prio[10] = 2; prio[11] = 1; prio[12] = 1; prio[80] = 0;
   TEST_CHECK(lv2_lwq_lock(&q, 10, &sig) == LV2_LWQ_QUEUED && lv2_lwq_lock(&q, 12, &sig) == LV2_LWQ_QUEUED);
   TEST_CHECK(lv2_lwq_lock(&q, 11, &sig) == LV2_LWQ_QUEUED && lv2_lwq_lock(&q, 80, &sig) == LV2_LWQ_QUEUED);
   TEST_CHECK(lv2_lwq_unlock(&q, 0, by_prio, NULL) == 80 && lv2_lwq_unlock(&q, 0, by_prio, NULL) == 12);
   TEST_CHECK(lv2_lwq_unlock(&q, 0, by_prio, NULL) == 11 && lv2_lwq_unlock(&q, 0, by_prio, NULL) == 10);

   /* A save state's signal */
   lv2_lwq_init(&q, LV2_LWQ_SIGNAL | LV2_LWQ_SIGNAL2);
   TEST_CHECK(lv2_lwq_signal(&q) == (LV2_LWQ_SIGNAL | LV2_LWQ_SIGNAL2) && lv2_lwq_trylock(&q) && !lv2_lwq_signal(&q));
}

#define THREADS 6
#define ROUNDS 20000

static lv2_lwq_t shared;
static retro_atomic_int_t handed[THREADS];
static retro_atomic_int_t inside;
static retro_atomic_int_t errors;
static unsigned long work; /* the owner's alone: plain memory */

static void locker(void *data)
{
   const unsigned slot = (unsigned)(size_t)data;
   const unsigned me   = slot / 16u;
   unsigned r;

   for (r = 0; r < ROUNDS; r++)
   {
      uint32_t sig = 0;
      const int got = lv2_lwq_lock(&shared, slot, &sig);
      int to;

      if (got == LV2_LWQ_QUEUED)
      {
         if (r % 3 == 0 && lv2_lwq_leave(&shared, slot))
            continue; /* timed out, never the owner */

         while (!retro_atomic_load_acquire_int(&handed[me]))
            sched_yield();
         retro_atomic_store_release_int(&handed[me], 0);
      }
      else if (got != LV2_LWQ_OWNED)
         retro_atomic_inc_int(&errors);

      if (retro_atomic_fetch_add_int(&inside, 1) != 0)
         retro_atomic_inc_int(&errors);
      work++;
      retro_atomic_dec_int(&inside);

      to = lv2_lwq_unlock(&shared, 0, NULL, NULL);
      if (to >= 0 && retro_atomic_exchange_int(&handed[to / 16], 1) != 0)
         retro_atomic_inc_int(&errors);
   }
}

static void threads(void)
{
   sthread_t *t[THREADS];
   unsigned i;

   /* Free: the signal left */
   lv2_lwq_init(&shared, LV2_LWQ_SIGNAL);
   retro_atomic_int_init(&inside, 0);
   retro_atomic_int_init(&errors, 0);
   for (i = 0; i < THREADS; i++)
      retro_atomic_int_init(&handed[i], 0);

   for (i = 0; i < THREADS; i++)
      t[i] = sthread_create(locker, (void*)(size_t)(i * 16u + 3u));
   for (i = 0; i < THREADS; i++)
      sthread_join(t[i]);

   TEST_CHECK(retro_atomic_load_acquire_int(&errors) == 0);
   TEST_CHECK(lv2_lwq_signal(&shared) == LV2_LWQ_SIGNAL && !lv2_lwq_any_waiter(&shared));
   printf("test_lv2_lwq: %d threads locking with timeouts: %lu times owned, never by two\n", THREADS, work);
}

int main(void)
{
   basics();
   threads();
   if (test_failures)
   {
      fprintf(stderr, "test_lv2_lwq: %d failed\n", test_failures);
      return 1;
   }
   printf("test_lv2_lwq: the signal taken or asleep, handed to the best sleeper, unlock2 told\n");
   return 0;
}
