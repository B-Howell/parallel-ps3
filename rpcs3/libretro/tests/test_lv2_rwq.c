/* An lv2 rwlock's owners and waiters in one word (lv2_rwq.c): read and
 * written at once or queued, a writer handed it before readers, the
 * readers let in together, a writer's timeout letting them in; a thread
 * that left as a reader and queues again as a writer never given the
 * read it left; and under
 * ThreadSanitizer threads reading and writing with timeouts at once -
 * never a writer with anyone else, every thread queued either given the
 * lock or out of the queue. C89. */

#include <stdlib.h>
#include <string.h>

#include "../../Emu/Cell/lv2/lv2_rwq.h"
#include "test_common.h"

#include <rthreads/rthreads.h>
#include <sched.h>

static int prio[LV2_RWQ_SLOTS];

static int by_prio(void *ctx, unsigned slot)
{
   (void)ctx;
   return prio[slot];
}

static int has(retro_u128_t w, unsigned slot)
{
   return slot < 64u ? (int)((w.lo >> slot) & 1u) : (int)((w.hi >> (slot - 64u)) & 1u);
}

/* A writer's unlock between a reader's look at the word and its joining,
 * when armed */
static int window_writer = -1;

void lv2_rwq_test_window(lv2_rwq_t *q)
{
   retro_u128_t w;
   if (window_writer >= 0)
   {
      lv2_rwq_wunlock(q, (unsigned)window_writer, NULL, NULL, &w);
      window_writer = -1;
   }
}

static void basics(void)
{
   lv2_rwq_t q;
   retro_u128_t w;

   lv2_rwq_init(&q, -1, 0);
   TEST_CHECK(lv2_rwq_rlock(&q, 1) == LV2_RWQ_OWNED && lv2_rwq_tryrlock(&q) == LV2_RWQ_OWNED);
   TEST_CHECK(lv2_rwq_readers(&q) == 2 && lv2_rwq_trywlock(&q, 3) == LV2_RWQ_BUSY);

   /* A writer queues behind the readers; a reader then queues behind it */
   TEST_CHECK(lv2_rwq_wlock(&q, 3) == LV2_RWQ_QUEUED && lv2_rwq_tryrlock(&q) == LV2_RWQ_BUSY);
   TEST_CHECK(lv2_rwq_rlock(&q, 70) == LV2_RWQ_QUEUED && lv2_rwq_rlock(&q, 70) == LV2_RWQ_BAD);
   TEST_CHECK(lv2_rwq_wunlock(&q, 3, NULL, NULL, &w) == LV2_RWQ_NOT_OWNER);
   TEST_CHECK(lv2_rwq_runlock(&q, NULL, NULL, &w) == 0 && !w.lo && !w.hi);
   /* The last reader hands it to the writer */
   TEST_CHECK(lv2_rwq_runlock(&q, NULL, NULL, &w) == 0 && has(w, 3) && lv2_rwq_writer(&q) == 3);
   TEST_CHECK(lv2_rwq_runlock(&q, NULL, NULL, &w) == LV2_RWQ_NOT_OWNER);
   TEST_CHECK(lv2_rwq_wlock(&q, 3) == LV2_RWQ_MINE && lv2_rwq_trywlock(&q, 3) == LV2_RWQ_MINE);

   /* Writers before readers, by priority, then the readers together */
   prio[9] = 5; prio[80] = 1;
   TEST_CHECK(lv2_rwq_wlock(&q, 9) == LV2_RWQ_QUEUED && lv2_rwq_rlock(&q, 71) == LV2_RWQ_QUEUED);
   TEST_CHECK(lv2_rwq_wlock(&q, 80) == LV2_RWQ_QUEUED);
   TEST_CHECK(lv2_rwq_wunlock(&q, 3, by_prio, NULL, &w) == 0 && has(w, 80) && !has(w, 9) && lv2_rwq_writer(&q) == 80);
   TEST_CHECK(lv2_rwq_wunlock(&q, 80, by_prio, NULL, &w) == 0 && has(w, 9) && lv2_rwq_writer(&q) == 9);
   TEST_CHECK(lv2_rwq_wunlock(&q, 9, by_prio, NULL, &w) == 0 && has(w, 70) && has(w, 71));
   TEST_CHECK(lv2_rwq_readers(&q) == 2 && lv2_rwq_writer(&q) == -1 && !lv2_rwq_any_waiter(&q));

   /* The last writer waiting times out: the readers behind it let in */
   TEST_CHECK(lv2_rwq_wlock(&q, 4) == LV2_RWQ_QUEUED && lv2_rwq_rlock(&q, 5) == LV2_RWQ_QUEUED);
   TEST_CHECK(lv2_rwq_wleave(&q, 4, NULL, NULL, &w) == 1 && has(w, 5) && lv2_rwq_readers(&q) == 3);
   TEST_CHECK(lv2_rwq_rleave(&q, 5) == 0 && lv2_rwq_wleave(&q, 5, NULL, NULL, &w) == 0);
   TEST_CHECK(lv2_rwq_runlock(&q, NULL, NULL, &w) == 0 && lv2_rwq_runlock(&q, NULL, NULL, &w) == 0);
   TEST_CHECK(lv2_rwq_runlock(&q, NULL, NULL, &w) == 0 && !lv2_rwq_busy(&q));
   TEST_CHECK(lv2_rwq_wlock(&q, 100) == LV2_RWQ_BAD);

   /* Left as a reader, queued again as a writer: the writer, not a reader */
   lv2_rwq_init(&q, 2, 0);
   TEST_CHECK(lv2_rwq_rlock(&q, 6) == LV2_RWQ_QUEUED && lv2_rwq_rleave(&q, 6));
   TEST_CHECK(lv2_rwq_wlock(&q, 6) == LV2_RWQ_QUEUED);
   TEST_CHECK(lv2_rwq_wunlock(&q, 2, NULL, NULL, &w) == 0 && has(w, 6) && lv2_rwq_writer(&q) == 6 && !lv2_rwq_readers(&q));

   /* The writer gives it up as a reader joins, seeing no reader: the
    * reader sees it free after it joined, and takes it */
   lv2_rwq_init(&q, 2, 0);
   window_writer = 2;
   TEST_CHECK(lv2_rwq_rlock(&q, 7) == LV2_RWQ_OWNED && lv2_rwq_readers(&q) == 1 && !lv2_rwq_any_waiter(&q));

   /* A save state's owners */
   lv2_rwq_init(&q, 42, 0);
   TEST_CHECK(lv2_rwq_writer(&q) == 42 && lv2_rwq_readers(&q) == 0);
   lv2_rwq_init(&q, -1, 7);
   TEST_CHECK(lv2_rwq_writer(&q) == -1 && lv2_rwq_readers(&q) == 7);
}

#define THREADS 6
#define ROUNDS 15000
/* Yields a queued thread waits before calling its wakeup lost */
#define STUCK  2000000L

static lv2_rwq_t shared;
static retro_atomic_int_t given[THREADS];
static retro_atomic_int_t readers_in;
static retro_atomic_int_t writers_in;
static retro_atomic_int_t errors;
static retro_atomic_int_t stuck;
static unsigned long wrote; /* the writer's alone: plain memory */

static void give(retro_u128_t w)
{
   unsigned i;
   for (i = 0; i < THREADS; i++)
   {
      if (has(w, i * 16u + 3u) && retro_atomic_exchange_int(&given[i], 1) != 0)
         retro_atomic_inc_int(&errors);
   }
}

static void worker(void *data)
{
   const unsigned me   = (unsigned)(size_t)data;
   const unsigned slot = me * 16u + 3u;
   unsigned r;

   for (r = 0; r < ROUNDS; r++)
   {
      const int write = (r + me) % 3 == 0;
      const int got   = write ? lv2_rwq_wlock(&shared, slot) : lv2_rwq_rlock(&shared, slot);
      retro_u128_t w;

      if (got == LV2_RWQ_QUEUED)
      {
         w.lo = 0;
         w.hi = 0;
         if (r % 4 == 1 && (write ? lv2_rwq_wleave(&shared, slot, NULL, NULL, &w) : lv2_rwq_rleave(&shared, slot)))
         {
            give(w);
            continue; /* timed out, never given it */
         }

         {
            long spins = 0;
            while (!retro_atomic_load_acquire_int(&given[me]))
            {
               if (++spins > STUCK || retro_atomic_load_acquire_int(&stuck))
               {
                  /* Never given it: every thread stops */
                  retro_atomic_store_release_int(&stuck, 1);
                  retro_atomic_inc_int(&errors);
                  return;
               }
               sched_yield();
            }
         }
         retro_atomic_store_release_int(&given[me], 0);
      }
      else if (got != LV2_RWQ_OWNED)
         retro_atomic_inc_int(&errors);

      if (write)
      {
         if (lv2_rwq_writer(&shared) != (int)slot || retro_atomic_fetch_add_int(&writers_in, 1) != 0
               || retro_atomic_load_acquire_int(&readers_in) != 0)
            retro_atomic_inc_int(&errors);
         wrote++;
         retro_atomic_dec_int(&writers_in);
         if (lv2_rwq_wunlock(&shared, slot, NULL, NULL, &w) != 0)
            retro_atomic_inc_int(&errors);
      }
      else
      {
         retro_atomic_inc_int(&readers_in);
         if (retro_atomic_load_acquire_int(&writers_in) != 0 || lv2_rwq_writer(&shared) >= 0)
            retro_atomic_inc_int(&errors);
         retro_atomic_dec_int(&readers_in);
         if (lv2_rwq_runlock(&shared, NULL, NULL, &w) != 0)
            retro_atomic_inc_int(&errors);
      }
      give(w);
   }
}

static void threads(void)
{
   sthread_t *t[THREADS];
   unsigned i;

   lv2_rwq_init(&shared, -1, 0);
   retro_atomic_int_init(&readers_in, 0);
   retro_atomic_int_init(&writers_in, 0);
   retro_atomic_int_init(&errors, 0);
   retro_atomic_int_init(&stuck, 0);
   for (i = 0; i < THREADS; i++)
      retro_atomic_int_init(&given[i], 0);

   for (i = 0; i < THREADS; i++)
      t[i] = sthread_create(worker, (void*)(size_t)i);
   for (i = 0; i < THREADS; i++)
      sthread_join(t[i]);

   TEST_CHECK(retro_atomic_load_acquire_int(&errors) == 0);
   TEST_CHECK(retro_atomic_load_acquire_int(&stuck) || !lv2_rwq_busy(&shared));
   printf("test_lv2_rwq: %d threads reading and writing with timeouts: %lu writes, never with anyone else\n", THREADS, wrote);
}

int main(void)
{
   basics();
   threads();
   if (test_failures)
   {
      fprintf(stderr, "test_lv2_rwq: %d failed\n", test_failures);
      return 1;
   }
   printf("test_lv2_rwq: read and written or queued, writers first, readers together, a writer's timeout letting them in\n");
   return 0;
}
