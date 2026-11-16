/* An lv2 event flag's bits and waiters with no lock (lv2_flagq.c): taken
 * now or waited for, the waiters a set satisfies woken best first with
 * the bits each takes gone for the next, a single-waiter flag refusing a
 * second, cancels, time-outs; and under ThreadSanitizer a token passed
 * round threads through the flag's bits, some timing out as they wait,
 * with others setting and taking a bit of their own meanwhile - never
 * lost, never taken twice. C89. */

#include <stdlib.h>
#include <string.h>

#include "../../Emu/Cell/lv2/lv2_flagq.h"
#include "test_common.h"

#include <rthreads/rthreads.h>
#include <sched.h>

#define AND_CLEAR (LV2_FLAGQ_AND | LV2_FLAGQ_CLEAR)
#define FQ_BIT(n) ((uint64_t)1 << (n))

static int prio[LV2_FLAGQ_SLOTS];

static int by_prio(void *ctx, unsigned slot)
{
   (void)ctx;
   return prio[slot];
}

static int woke(retro_u128_t w, unsigned slot)
{
   return slot < 64u ? (int)((w.lo >> slot) & 1u) : (int)((w.hi >> (slot - 64u)) & 1u);
}

static lv2_flagq_t q;

/* A set between a waiter's look at the bits and its joining, when armed */
static uint64_t window_bits;

void lv2_flagq_test_window(lv2_flagq_t *fq)
{
   retro_u128_t w;
   if (window_bits)
   {
      lv2_flagq_set(fq, window_bits, NULL, NULL, &w);
      window_bits = 0;
   }
}

static void basics(void)
{
   uint64_t r = 0;
   retro_u128_t w;

   TEST_CHECK(lv2_flagq_mode_ok(LV2_FLAGQ_OR | LV2_FLAGQ_CLEAR_ALL) && !lv2_flagq_mode_ok(3) && !lv2_flagq_mode_ok(0x31));

   lv2_flagq_init(&q, 0x5);
   TEST_CHECK(lv2_flagq_try(&q, 0x1, LV2_FLAGQ_AND, &r) && r == 0x5 && lv2_flagq_pattern(&q) == 0x5);
   TEST_CHECK(!lv2_flagq_try(&q, 0x3, LV2_FLAGQ_AND, &r) && r == 0x5);
   TEST_CHECK(lv2_flagq_try(&q, 0x3, LV2_FLAGQ_OR | LV2_FLAGQ_CLEAR, &r) && lv2_flagq_pattern(&q) == 0x4);
   TEST_CHECK(lv2_flagq_wait(&q, 7, 0x4, LV2_FLAGQ_OR | LV2_FLAGQ_CLEAR_ALL, 0, &r) == LV2_FLAGQ_GOT && lv2_flagq_pattern(&q) == 0);

   /* Three wait for bit 1, taking it: a set wakes the best, and only it */
   prio[3] = 2; prio[90] = 1; prio[5] = 1;
   TEST_CHECK(lv2_flagq_wait(&q, 3, 0x2, AND_CLEAR, 0, &r) == LV2_FLAGQ_WAITING);
   TEST_CHECK(lv2_flagq_wait(&q, 90, 0x2, AND_CLEAR, 0, &r) == LV2_FLAGQ_WAITING);
   TEST_CHECK(lv2_flagq_wait(&q, 5, 0x2, AND_CLEAR, 0, &r) == LV2_FLAGQ_WAITING);
   TEST_CHECK(lv2_flagq_wait(&q, 5, 0x2, AND_CLEAR, 0, &r) == LV2_FLAGQ_BAD);
   TEST_CHECK(lv2_flagq_busy(&q));
   lv2_flagq_set(&q, 0x2, by_prio, NULL, &w);
   TEST_CHECK(woke(w, 90) && !woke(w, 5) && !woke(w, 3));
   TEST_CHECK(lv2_flagq_verdict(&q, 90) == LV2_FLAGQ_TAKEN && lv2_flagq_result(&q, 90) == 0x2);
   TEST_CHECK(lv2_flagq_verdict(&q, 5) == LV2_FLAGQ_PENDING && lv2_flagq_pattern(&q) == 0);
   /* In the order they came among equals; a set not satisfying any wakes none */
   lv2_flagq_set(&q, 0x8, by_prio, NULL, &w);
   TEST_CHECK(!w.lo && !w.hi && lv2_flagq_pattern(&q) == 0x8);
   lv2_flagq_set(&q, 0x2, by_prio, NULL, &w);
   TEST_CHECK(woke(w, 5) && !woke(w, 3) && lv2_flagq_pattern(&q) == 0x8);

   /* One times out; a cancel gives the rest the pattern */
   TEST_CHECK(lv2_flagq_wait(&q, 70, 0x10, LV2_FLAGQ_OR, 0, &r) == LV2_FLAGQ_WAITING);
   TEST_CHECK(lv2_flagq_timeout(&q, 70) == LV2_FLAGQ_TIMEDOUT && lv2_flagq_result(&q, 70) == 0x8);
   TEST_CHECK(lv2_flagq_cancel(&q, &w) == 1 && woke(w, 3));
   TEST_CHECK(lv2_flagq_verdict(&q, 3) == LV2_FLAGQ_CANCELED && lv2_flagq_result(&q, 3) == 0x8);
   TEST_CHECK(!lv2_flagq_busy(&q));
   /* Woken before its time ran out: its verdict stands */
   TEST_CHECK(lv2_flagq_wait(&q, 4, 0x10, LV2_FLAGQ_OR, 0, &r) == LV2_FLAGQ_WAITING);
   lv2_flagq_set(&q, 0x10, NULL, NULL, &w);
   TEST_CHECK(lv2_flagq_timeout(&q, 4) == LV2_FLAGQ_TAKEN && lv2_flagq_result(&q, 4) == 0x18);

   /* A set that came as it joined, seeing no waiter: the waiter sees the
    * bits after it joined, and takes them */
   lv2_flagq_init(&q, 0);
   window_bits = 0x40;
   TEST_CHECK(lv2_flagq_wait(&q, 8, 0x40, AND_CLEAR, 0, &r) == LV2_FLAGQ_GOT && r == 0x40);
   TEST_CHECK(lv2_flagq_pattern(&q) == 0 && !lv2_flagq_busy(&q));

   /* A single-waiter flag: the second refused, out of the waiters */
   lv2_flagq_init(&q, 0);
   TEST_CHECK(lv2_flagq_wait(&q, 1, 0x1, LV2_FLAGQ_AND, 1, &r) == LV2_FLAGQ_WAITING);
   TEST_CHECK(lv2_flagq_wait(&q, 2, 0x1, LV2_FLAGQ_AND, 1, &r) == LV2_FLAGQ_PERM);
   TEST_CHECK(lv2_flagq_leave(&q, 1) && !lv2_flagq_leave(&q, 2) && !lv2_flagq_busy(&q));

   /* Put back by a set that lost the bits keeps its place: not seen here
    * but in the waiter set's tickets */
   lv2_flagq_init(&q, 0);
   TEST_CHECK(lv2_flagq_wait(&q, 6, 0x1, AND_CLEAR, 0, &r) == LV2_FLAGQ_WAITING);
   TEST_CHECK(lv2_flagq_wait(&q, 9, 0x1, AND_CLEAR, 0, &r) == LV2_FLAGQ_WAITING);
   TEST_CHECK(lv2_waitset_leave(&q.ws, 6) && lv2_waitset_rejoin(&q.ws, 6) && !lv2_waitset_rejoin(&q.ws, 6));
   lv2_flagq_set(&q, 0x1, NULL, NULL, &w);
   TEST_CHECK(woke(w, 6) && !woke(w, 9));
}

/* A token goes round RING threads: thread i waits for bit i, takes it and
 * sets bit i+1. With one token no bit is set twice, so a lost wakeup
 * stops the ring and a token taken twice is two threads holding it */
#define RING   5
#define ROUNDS 1500
/* Yields a waiter waits before calling its wakeup lost */
#define STUCK  2000000L
#define NOISE  2

static retro_atomic_int_t holding;
static retro_atomic_int_t errors;
static retro_atomic_int_t passes;
static retro_atomic_int_t stop_noise;
static retro_atomic_int_t stuck;

static void ring(void *data)
{
   const unsigned i    = (unsigned)(size_t)data;
   const unsigned slot = i * 7u + 1u;
   const uint64_t mine = FQ_BIT(i);
   unsigned r;

   for (r = 0; r < ROUNDS; r++)
   {
      uint64_t result = 0;
      int got = lv2_flagq_wait(&q, slot, mine, AND_CLEAR, 0, &result);
      retro_u128_t w;

      while (got == LV2_FLAGQ_WAITING)
      {
         if (retro_atomic_load_acquire_int(&stuck))
            return;
         if (r % 4 == 1)
         {
            /* Its time runs out: taken all the same, or round again */
            sched_yield();
            got = lv2_flagq_timeout(&q, slot) == LV2_FLAGQ_TAKEN ? LV2_FLAGQ_GOT : -9;
         }
         else
         {
            long spins = 0;
            while (lv2_flagq_verdict(&q, slot) == LV2_FLAGQ_PENDING)
            {
               if (++spins > STUCK || retro_atomic_load_acquire_int(&stuck))
               {
                  /* Its wakeup lost: the ring stops */
                  retro_atomic_store_release_int(&stuck, 1);
                  retro_atomic_inc_int(&errors);
                  return;
               }
               sched_yield();
            }
            got = lv2_flagq_verdict(&q, slot) == LV2_FLAGQ_TAKEN ? LV2_FLAGQ_GOT : -9;
         }

         if (got == LV2_FLAGQ_GOT)
            result = lv2_flagq_result(&q, slot);
         else
            got = lv2_flagq_wait(&q, slot, mine, AND_CLEAR, 0, &result);
      }

      if (got != LV2_FLAGQ_GOT || !(result & mine))
         retro_atomic_inc_int(&errors);
      if (retro_atomic_fetch_add_int(&holding, 1) != 0)
         retro_atomic_inc_int(&errors);
      retro_atomic_inc_int(&passes);
      retro_atomic_dec_int(&holding);

      lv2_flagq_set(&q, FQ_BIT((i + 1u) % RING), NULL, NULL, &w);
   }
}

/* Others take and set bit 40 meanwhile: claims, puts back and time-outs
 * among the ring's */
static void noise(void *data)
{
   const unsigned slot = 60u + (unsigned)(size_t)data;
   retro_u128_t w;

   while (!retro_atomic_load_acquire_int(&stop_noise))
   {
      uint64_t result;
      lv2_flagq_set(&q, FQ_BIT(40), NULL, NULL, &w);
      if (lv2_flagq_wait(&q, slot, FQ_BIT(40) | FQ_BIT(41), LV2_FLAGQ_OR | LV2_FLAGQ_CLEAR, 0, &result) == LV2_FLAGQ_WAITING)
         lv2_flagq_timeout(&q, slot);
      lv2_flagq_clear(&q, ~FQ_BIT(41));
   }
}

static void threads(void)
{
   sthread_t *t[RING + NOISE];
   unsigned i;

   lv2_flagq_init(&q, FQ_BIT(0));
   retro_atomic_int_init(&holding, 0);
   retro_atomic_int_init(&errors, 0);
   retro_atomic_int_init(&passes, 0);
   retro_atomic_int_init(&stop_noise, 0);
   retro_atomic_int_init(&stuck, 0);

   for (i = 0; i < NOISE; i++)
      t[RING + i] = sthread_create(noise, (void*)(size_t)i);
   for (i = 0; i < RING; i++)
      t[i] = sthread_create(ring, (void*)(size_t)i);
   for (i = 0; i < RING; i++)
      sthread_join(t[i]);
   retro_atomic_store_release_int(&stop_noise, 1);
   for (i = 0; i < NOISE; i++)
      sthread_join(t[RING + i]);

   TEST_CHECK(retro_atomic_load_acquire_int(&errors) == 0);
   TEST_CHECK(retro_atomic_load_acquire_int(&passes) == RING * ROUNDS);
   /* The token back where it started, nothing claimed */
   TEST_CHECK((lv2_flagq_pattern(&q) & ((FQ_BIT(RING) - 1u))) == FQ_BIT(0));
   TEST_CHECK(!lv2_flagq_busy(&q));
   printf("test_lv2_flagq: a token round %d threads %d times with time-outs and others meanwhile, never lost or held twice\n",
         RING, ROUNDS);
}

int main(void)
{
   basics();
   threads();
   if (test_failures)
   {
      fprintf(stderr, "test_lv2_flagq: %d failed\n", test_failures);
      return 1;
   }
   printf("test_lv2_flagq: taken or waited for, woken best first, single waiter, cancels and time-outs\n");
   return 0;
}
