/* An lv2 event flag's bits and waiters, lock-free (lv2_flagq.h). C89. */

#include "lv2_flagq.h"

#include <rthreads/retro_waitaddr.h>

#ifdef LV2_FLAGQ_TEST_WINDOW
void LV2_FLAGQ_TEST_WINDOW(lv2_flagq_t *q);
#endif

#define FQ_ONE ((uint64_t)1)

static int fq_bit(retro_u128_t v, unsigned slot)
{
   return slot < 64u ? (int)((v.lo >> slot) & 1u) : (int)((v.hi >> (slot - 64u)) & 1u);
}

static void fq_set_bit(retro_u128_t *v, unsigned slot)
{
   if (slot < 64u)
      v->lo |= FQ_ONE << slot;
   else
      v->hi |= FQ_ONE << (slot - 64u);
}

/* As LV2 checks a pattern: whether it satisfies @want in @mode, and the
 * pattern as the waiter leaves it */
static int fq_check(uint64_t *pattern, uint64_t want, unsigned mode)
{
   if ((mode & 0xfu) == LV2_FLAGQ_AND && (*pattern & want) != want)
      return 0;
   if ((mode & 0xfu) == LV2_FLAGQ_OR && (*pattern & want) == 0)
      return 0;

   if ((mode & ~0xfu) == LV2_FLAGQ_CLEAR)
      *pattern &= ~want;
   else if ((mode & ~0xfu) == LV2_FLAGQ_CLEAR_ALL)
      *pattern = 0;
   return 1;
}

void lv2_flagq_init(lv2_flagq_t *q, uint64_t pattern)
{
   unsigned i;

   retro_atomic_64_init(&q->pattern, (int64_t)pattern);
   lv2_waitset_init(&q->ws);
   retro_atomic_int_init(&q->claiming, 0);
   for (i = 0; i < LV2_FLAGQ_SLOTS; i++)
   {
      retro_atomic_64_init(&q->want[i], 0);
      retro_atomic_int_init(&q->mode[i], 0);
      retro_atomic_int_init(&q->timed_out[i], 0);
      retro_atomic_int_init(&q->verdict[i], 0);
      retro_atomic_64_init(&q->result[i], 0);
   }
}

int lv2_flagq_mode_ok(unsigned mode)
{
   const unsigned how   = mode & 0xfu;
   const unsigned clear = mode & ~0xfu;

   return (how == LV2_FLAGQ_AND || how == LV2_FLAGQ_OR)
      && (clear == 0 || clear == LV2_FLAGQ_CLEAR || clear == LV2_FLAGQ_CLEAR_ALL);
}

int lv2_flagq_try(lv2_flagq_t *q, uint64_t want, unsigned mode, uint64_t *result)
{
   for (;;)
   {
      const int64_t seen = retro_atomic_load_acquire_64(&q->pattern);
      uint64_t pattern   = (uint64_t)seen;

      *result = pattern;
      if (!fq_check(&pattern, want, mode))
         return 0;
      if (pattern == (uint64_t)seen
            || retro_atomic_cas_64(&q->pattern, seen, (int64_t)pattern))
         return 1;
   }
}

/* Whether the bits as they are would satisfy the waiter in @slot */
static int fq_would(lv2_flagq_t *q, unsigned slot)
{
   uint64_t pattern = (uint64_t)retro_atomic_load_acquire_64(&q->pattern);
   return fq_check(&pattern, (uint64_t)retro_atomic_load_acquire_64(&q->want[slot]),
         (unsigned)retro_atomic_load_acquire_int(&q->mode[slot]));
}

int lv2_flagq_wait(lv2_flagq_t *q, unsigned slot, uint64_t want, unsigned mode,
      int single, uint64_t *result)
{
   if (slot >= LV2_FLAGQ_SLOTS)
      return LV2_FLAGQ_BAD;

   if (lv2_flagq_try(q, want, mode, result))
      return LV2_FLAGQ_GOT;

   /* What it waits for, and no result yet, before it is a waiter: a set
    * that sees its bit reads them */
   retro_atomic_store_release_64(&q->want[slot], (int64_t)want);
   retro_atomic_store_release_int(&q->mode[slot], (int)mode);
   retro_atomic_store_release_int(&q->timed_out[slot], 0);
   retro_atomic_store_release_int(&q->verdict[slot], LV2_FLAGQ_PENDING);

#ifdef LV2_FLAGQ_TEST_WINDOW
   /* The test sets the bits here: a set that saw no waiter */
   LV2_FLAGQ_TEST_WINDOW(q);
#endif

   if (!lv2_waitset_join(&q->ws, slot))
      return LV2_FLAGQ_BAD;

   if (single)
   {
      /* Of two that wait at once, the later leaves */
      const retro_u128_t v = retro_atomic_load_acquire_128(&q->ws.waiting);
      const int mine       = retro_atomic_load_acquire_int(&q->ws.ticket[slot]);
      unsigned s;

      for (s = 0; s < LV2_FLAGQ_SLOTS; s++)
      {
         if (s == slot || !fq_bit(v, s))
            continue;
         if ((int)((unsigned)retro_atomic_load_acquire_int(&q->ws.ticket[s]) - (unsigned)mine) < 0)
            return lv2_waitset_leave(&q->ws, slot) ? LV2_FLAGQ_PERM : LV2_FLAGQ_WAITING;
      }
   }

   /* The bits again, after joining: a set before it did not see it */
   for (;;)
   {
      if (!fq_would(q, slot))
         return LV2_FLAGQ_WAITING;
      if (!lv2_waitset_leave(&q->ws, slot))
         return LV2_FLAGQ_WAITING; /* claimed: its result comes */
      if (lv2_flagq_try(q, want, mode, result))
         return LV2_FLAGQ_GOT;
      /* Gone again meanwhile: back, in its place */
      lv2_waitset_rejoin(&q->ws, slot);
   }
}

int lv2_flagq_leave(lv2_flagq_t *q, unsigned slot)
{
   return lv2_waitset_leave(&q->ws, slot);
}

/* A waker gives the claimed waiter in @slot its result: the verdict last,
 * then a waiter whose time ran out, waiting for it, woken */
static void fq_give(lv2_flagq_t *q, unsigned slot, int verdict, uint64_t result)
{
   retro_atomic_store_release_64(&q->result[slot], (int64_t)result);
   retro_atomic_store_release_int(&q->verdict[slot], verdict);
   retro_atomic_thread_fence_seq_cst();
   if (retro_atomic_load_acquire_int(&q->timed_out[slot]))
      retro_waitaddr_notify_all((const void*)&q->verdict[slot]);
}

int lv2_flagq_await(lv2_flagq_t *q, unsigned slot)
{
   for (;;)
   {
      const int verdict = retro_atomic_load_acquire_int(&q->verdict[slot]);
      retro_waitaddr_t w;

      if (verdict != LV2_FLAGQ_PENDING)
         return verdict;

      w.addr = (const void*)&q->verdict[slot];
      w.old  = (uint32_t)LV2_FLAGQ_PENDING;
      retro_waitaddr_wait(&w, 1, -1);
   }
}

int lv2_flagq_timeout(lv2_flagq_t *q, unsigned slot)
{
   if (slot >= LV2_FLAGQ_SLOTS)
      return LV2_FLAGQ_TIMEDOUT;

   /* Its time-out told before it leaves: a waker that claimed it first
    * reads it after giving its verdict, and wakes it */
   retro_atomic_store_release_int(&q->timed_out[slot], 1);
   retro_atomic_thread_fence_seq_cst();

   if (lv2_waitset_leave(&q->ws, slot))
   {
      retro_atomic_store_release_64(&q->result[slot], retro_atomic_load_acquire_64(&q->pattern));
      retro_atomic_store_release_int(&q->verdict[slot], LV2_FLAGQ_TIMEDOUT);
      return LV2_FLAGQ_TIMEDOUT;
   }

   return lv2_flagq_await(q, slot);
}

int lv2_flagq_verdict(lv2_flagq_t *q, unsigned slot)
{
   return retro_atomic_load_acquire_int(&q->verdict[slot]);
}

uint64_t lv2_flagq_result(lv2_flagq_t *q, unsigned slot)
{
   return (uint64_t)retro_atomic_load_acquire_64(&q->result[slot]);
}

/* The best waiter in @v: by rank, then by ticket (compared by their
 * difference: they wrap). -1 when none */
static int fq_best(lv2_flagq_t *q, retro_u128_t v, lv2_flagq_rank_t rank, void *ctx)
{
   int best      = -1;
   int best_rank = 0;
   int best_tick = 0;
   unsigned s;

   for (s = 0; s < LV2_FLAGQ_SLOTS; s++)
   {
      int r;
      int t;

      if (!fq_bit(v, s))
         continue;
      r = rank ? rank(ctx, s) : 0;
      t = retro_atomic_load_acquire_int(&q->ws.ticket[s]);
      if (best < 0 || r < best_rank
            || (r == best_rank && (int)((unsigned)t - (unsigned)best_tick) < 0))
      {
         best      = (int)s;
         best_rank = r;
         best_tick = t;
      }
   }
   return best;
}

void lv2_flagq_set(lv2_flagq_t *q, uint64_t bits, lv2_flagq_rank_t rank, void *ctx,
      retro_u128_t *woken)
{
   retro_u128_t tried;

   woken->lo = 0;
   woken->hi = 0;
   tried.lo  = 0;
   tried.hi  = 0;

   /* Claiming from here: a destroy that looks now is refused */
   retro_atomic_fetch_add_int(&q->claiming, 1);
   retro_atomic_fetch_or_64(&q->pattern, (int64_t)bits);
   /* The bits before the waiters: a waiter that joined after this sees
    * them itself */
   lv2_waitset_fence();

   for (;;)
   {
      retro_u128_t v = retro_atomic_load_acquire_128(&q->ws.waiting);
      int best;
      unsigned slot;

      v.lo &= ~tried.lo;
      v.hi &= ~tried.hi;
      best = fq_best(q, v, rank, ctx);
      if (best < 0)
         break;
      slot = (unsigned)best;
      fq_set_bit(&tried, slot);

      for (;;)
      {
         uint64_t result;

         if (!fq_would(q, slot))
            break;
         /* Claimed, or gone (left, or another set has it) */
         if (!lv2_waitset_leave(&q->ws, slot))
            break;

         if (lv2_flagq_try(q, (uint64_t)retro_atomic_load_acquire_64(&q->want[slot]),
                  (unsigned)retro_atomic_load_acquire_int(&q->mode[slot]), &result))
         {
            fq_give(q, slot, LV2_FLAGQ_TAKEN, result);
            fq_set_bit(woken, slot);
            break;
         }

         /* The bits went meanwhile. A waiter whose time ran out is given
          * it; else it is put back, and looked at again */
         if (retro_atomic_load_acquire_int(&q->timed_out[slot]))
         {
            fq_give(q, slot, LV2_FLAGQ_TIMEDOUT, lv2_flagq_pattern(q));
            break;
         }

         lv2_waitset_rejoin(&q->ws, slot);

         /* Its time ran out as it was put back: given it here, unless it
          * left on its own */
         if (retro_atomic_load_acquire_int(&q->timed_out[slot]))
         {
            if (lv2_waitset_leave(&q->ws, slot))
               fq_give(q, slot, LV2_FLAGQ_TIMEDOUT, lv2_flagq_pattern(q));
            break;
         }
      }
   }

   retro_atomic_fetch_sub_int(&q->claiming, 1);
}

unsigned lv2_flagq_cancel(lv2_flagq_t *q, retro_u128_t *woken)
{
   unsigned count = 0;
   uint64_t pattern;
   unsigned s;

   retro_atomic_fetch_add_int(&q->claiming, 1);
   retro_atomic_thread_fence_seq_cst();

   *woken  = lv2_waitset_claim_all(&q->ws);
   pattern = lv2_flagq_pattern(q);

   for (s = 0; s < LV2_FLAGQ_SLOTS; s++)
   {
      if (!fq_bit(*woken, s))
         continue;
      fq_give(q, s, LV2_FLAGQ_CANCELED, pattern);
      count++;
   }

   retro_atomic_fetch_sub_int(&q->claiming, 1);
   return count;
}

void lv2_flagq_clear(lv2_flagq_t *q, uint64_t bits)
{
   retro_atomic_fetch_and_64(&q->pattern, (int64_t)bits);
}

uint64_t lv2_flagq_pattern(lv2_flagq_t *q)
{
   return (uint64_t)retro_atomic_load_acquire_64(&q->pattern);
}

int lv2_flagq_busy(lv2_flagq_t *q)
{
   retro_atomic_thread_fence_seq_cst();
   return lv2_waitset_any(&q->ws) || retro_atomic_load_acquire_int(&q->claiming) != 0;
}
