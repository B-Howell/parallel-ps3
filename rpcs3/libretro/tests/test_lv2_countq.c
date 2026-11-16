/* An lv2 semaphore's count or waiters in one word (lv2_countq.c): counts
 * taken and posted, waiters given counts in the order they came or by
 * priority, the most kept as LV2 has it, and under ThreadSanitizer
 * waiters and posters at once with timeouts - every count posted is
 * either taken, given to one waiter or still counted: none lost, none
 * given twice. C89. */

#include <stdlib.h>
#include <string.h>

#include "../../Emu/Cell/lv2/lv2_countq.h"
#include "test_common.h"

#include <rthreads/rthreads.h>
#include <sched.h>

static int prio[LV2_COUNTQ_SLOTS];

static int by_prio(void *ctx, unsigned slot)
{
   (void)ctx;
   return prio[slot];
}

static void basics(void)
{
   lv2_countq_t q;
   retro_u128_t woken;

   lv2_countq_init(&q, 2);
   TEST_CHECK(lv2_countq_value(&q) == 2);
   TEST_CHECK(lv2_countq_trywait(&q) && lv2_countq_wait(&q, 3) == 1);
   TEST_CHECK(!lv2_countq_trywait(&q) && lv2_countq_value(&q) == 0);

   /* No count: they wait, in the same change */
   TEST_CHECK(lv2_countq_wait(&q, 3) == 0 && lv2_countq_wait(&q, 70) == 0 && lv2_countq_wait(&q, 9) == 0);
   TEST_CHECK(lv2_countq_wait(&q, 9) == -1 && lv2_countq_wait(&q, 127) == -1);
   TEST_CHECK(lv2_countq_any_waiter(&q) && lv2_countq_waits(&q, 70) && lv2_countq_value(&q) == 0);

   /* Posts give to the waiters in the order they came */
   TEST_CHECK(lv2_countq_post(&q, 1, 1, NULL, NULL, &woken) == LV2_COUNTQ_POSTED);
   TEST_CHECK(woken.lo == ((uint64_t)1 << 3) && woken.hi == 0);
   /* A waiter given a count cannot leave; one not given can */
   TEST_CHECK(!lv2_countq_leave(&q, 3));
   TEST_CHECK(lv2_countq_leave(&q, 9));
   /* With one waiter left, as many as the most and one more may be posted:
    * one to it, the rest counted */
   TEST_CHECK(lv2_countq_post(&q, 3, 1, NULL, NULL, &woken) == LV2_COUNTQ_BUSY);
   TEST_CHECK(lv2_countq_post(&q, 2, 1, NULL, NULL, &woken) == LV2_COUNTQ_POSTED);
   TEST_CHECK(woken.lo == 0 && woken.hi == ((uint64_t)1 << 6));
   TEST_CHECK(!lv2_countq_any_waiter(&q) && lv2_countq_value(&q) == 1);
   /* Over the most */
   TEST_CHECK(lv2_countq_post(&q, 1, 1, NULL, NULL, &woken) == LV2_COUNTQ_BUSY);
   TEST_CHECK(lv2_countq_value(&q) == 1);

   /* By priority, then in the order they came */
   lv2_countq_init(&q, 0);
   prio[5] = 3; prio[6] = 1; prio[7] = 1; prio[8] = 0;
   TEST_CHECK(lv2_countq_wait(&q, 5) == 0 && lv2_countq_wait(&q, 7) == 0 && lv2_countq_wait(&q, 6) == 0 && lv2_countq_wait(&q, 8) == 0);
   TEST_CHECK(lv2_countq_post(&q, 3, 10, by_prio, NULL, &woken) == LV2_COUNTQ_POSTED);
   TEST_CHECK(woken.lo == (((uint64_t)1 << 8) | ((uint64_t)1 << 7) | ((uint64_t)1 << 6)));
   TEST_CHECK(lv2_countq_waits(&q, 5));

   /* A count past an s32's */
   lv2_countq_init(&q, 0x7fffffffu);
   TEST_CHECK(lv2_countq_post(&q, 0x7fffffffu, 0xfffffffeu, NULL, NULL, &woken) == LV2_COUNTQ_POSTED);
   TEST_CHECK(lv2_countq_value(&q) == 0xfffffffeu);
}

#define WAITERS 8
#define POSTERS 2
#define ROUNDS 20000

static lv2_countq_t shared;
static retro_atomic_int_t given[WAITERS];
static retro_atomic_int_t got;
static retro_atomic_int_t posted;
static retro_atomic_int_t errors;
static retro_atomic_int_t stop;

static void waiter(void *data)
{
   const unsigned slot = (unsigned)(size_t)data;
   const unsigned me   = slot / 15u;
   unsigned r;

   for (r = 0; r < ROUNDS; r++)
   {
      const int taken = lv2_countq_wait(&shared, slot);
      if (taken < 0)
         retro_atomic_inc_int(&errors);
      if (taken > 0)
      {
         retro_atomic_inc_int(&got);
         continue;
      }

      if (r & 1)
         sched_yield();

      if (lv2_countq_leave(&shared, slot))
         continue; /* timed out, nothing taken */

      /* Given a count: woken */
      while (!retro_atomic_load_acquire_int(&given[me]))
         sched_yield();
      retro_atomic_store_release_int(&given[me], 0);
      retro_atomic_inc_int(&got);
   }
}

static void poster(void *data)
{
   (void)data;
   while (!retro_atomic_load_acquire_int(&stop))
   {
      retro_u128_t woken;
      unsigned slot;

      if (lv2_countq_post(&shared, 1, 4, NULL, NULL, &woken) != LV2_COUNTQ_POSTED)
      {
         sched_yield();
         continue;
      }
      retro_atomic_inc_int(&posted);

      for (slot = 0; slot < LV2_COUNTQ_SLOTS; slot++)
      {
         const int set = slot < 64u ? (int)((woken.lo >> slot) & 1u) : (int)((woken.hi >> (slot - 64u)) & 1u);
         if (set && retro_atomic_exchange_int(&given[slot / 15u], 1) != 0)
            retro_atomic_inc_int(&errors);
      }
   }
}

static void threads(void)
{
   sthread_t *w[WAITERS], *p[POSTERS];
   unsigned i;

   lv2_countq_init(&shared, 0);
   retro_atomic_int_init(&got, 0);
   retro_atomic_int_init(&posted, 0);
   retro_atomic_int_init(&errors, 0);
   retro_atomic_int_init(&stop, 0);
   for (i = 0; i < WAITERS; i++)
      retro_atomic_int_init(&given[i], 0);

   for (i = 0; i < POSTERS; i++)
      p[i] = sthread_create(poster, NULL);
   for (i = 0; i < WAITERS; i++)
      w[i] = sthread_create(waiter, (void*)(size_t)(i * 15u));
   for (i = 0; i < WAITERS; i++)
      sthread_join(w[i]);
   retro_atomic_store_release_int(&stop, 1);
   for (i = 0; i < POSTERS; i++)
      sthread_join(p[i]);

   /* Every count posted: taken, given, or still counted */
   TEST_CHECK(retro_atomic_load_acquire_int(&errors) == 0);
   TEST_CHECK((uint64_t)retro_atomic_load_acquire_int(&posted)
         == (uint64_t)retro_atomic_load_acquire_int(&got) + lv2_countq_value(&shared));
   TEST_CHECK(!lv2_countq_any_waiter(&shared));
   printf("test_lv2_countq: %d waiters with timeouts, %d posters: %d counts posted, %d taken or given, %u left counted\n",
         WAITERS, POSTERS, retro_atomic_load_acquire_int(&posted), retro_atomic_load_acquire_int(&got),
         (unsigned)lv2_countq_value(&shared));
}

int main(void)
{
   basics();
   threads();
   if (test_failures)
   {
      fprintf(stderr, "test_lv2_countq: %d failed\n", test_failures);
      return 1;
   }
   printf("test_lv2_countq: counts and waiters in one word, in order and by priority, the most as LV2 keeps it\n");
   return 0;
}
