/* An lv2 semaphore's count or waiters, one word (lv2_countq.h). C89. */

#include "lv2_countq.h"

#define CQ_COUNT_FLAG ((uint64_t)1 << 63)
#define CQ_ONE        ((uint64_t)1)

static int cq_is_count(retro_u128_t w)
{
   return (w.hi & CQ_COUNT_FLAG) != 0;
}

static retro_u128_t cq_count(uint64_t count)
{
   retro_u128_t w;
   w.lo = count;
   w.hi = count ? CQ_COUNT_FLAG : 0;
   return w;
}

static int cq_bit(retro_u128_t w, unsigned slot)
{
   return slot < 64u ? (int)((w.lo >> slot) & 1u) : (int)((w.hi >> (slot - 64u)) & 1u);
}

static retro_u128_t cq_with(retro_u128_t w, unsigned slot, int set)
{
   uint64_t *half   = slot < 64u ? &w.lo : &w.hi;
   const uint64_t m = CQ_ONE << (slot % 64u);
   if (set)
      *half |= m;
   else
      *half &= ~m;
   return w;
}

static unsigned cq_popcount(uint64_t v)
{
   unsigned n = 0;
   while (v)
   {
      v &= v - 1u;
      n++;
   }
   return n;
}

void lv2_countq_init(lv2_countq_t *q, uint64_t count)
{
   unsigned i;
   retro_atomic_128_init(&q->word, cq_count(count));
   retro_atomic_int_init(&q->next_ticket, 0);
   for (i = 0; i < LV2_COUNTQ_SLOTS; i++)
      retro_atomic_int_init(&q->ticket[i], 0);
}

int lv2_countq_trywait(lv2_countq_t *q)
{
   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      if (!cq_is_count(w))
         return 0;
      if (retro_atomic_cas_128(&q->word, w, cq_count(w.lo - 1u)))
         return 1;
   }
}

int lv2_countq_wait(lv2_countq_t *q, unsigned slot)
{
   int ticket;

   if (slot >= LV2_COUNTQ_SLOTS)
      return -1;

   ticket = retro_atomic_fetch_add_int(&q->next_ticket, 1);

   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);

      if (cq_is_count(w))
      {
         if (retro_atomic_cas_128(&q->word, w, cq_count(w.lo - 1u)))
            return 1;
         continue;
      }

      if (cq_bit(w, slot))
         return -1;

      /* The ticket before the bit: a post that sees the bit reads it */
      retro_atomic_store_release_int(&q->ticket[slot], ticket);
      if (retro_atomic_cas_128(&q->word, w, cq_with(w, slot, 1)))
         return 0;
   }
}

int lv2_countq_leave(lv2_countq_t *q, unsigned slot)
{
   if (slot >= LV2_COUNTQ_SLOTS)
      return 0;

   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      if (cq_is_count(w) || !cq_bit(w, slot))
         return 0;
      if (retro_atomic_cas_128(&q->word, w, cq_with(w, slot, 0)))
         return 1;
   }
}

/* The best waiter in @w: by rank, then by ticket (which wraps: compared
 * by difference). -1 when none */
static int cq_best(lv2_countq_t *q, retro_u128_t w, lv2_countq_rank_t rank, void *ctx)
{
   int best      = -1;
   int best_rank = 0;
   int best_tick = 0;
   unsigned slot;

   for (slot = 0; slot < LV2_COUNTQ_SLOTS; slot++)
   {
      int r;
      int t;

      if (!cq_bit(w, slot))
         continue;

      r = rank ? rank(ctx, slot) : 0;
      t = retro_atomic_load_acquire_int(&q->ticket[slot]);
      if (best < 0 || r < best_rank
            || (r == best_rank && (int)((unsigned)t - (unsigned)best_tick) < 0))
      {
         best      = (int)slot;
         best_rank = r;
         best_tick = t;
      }
   }
   return best;
}

int lv2_countq_post(lv2_countq_t *q, uint64_t count, uint64_t max,
      lv2_countq_rank_t rank, void *ctx, retro_u128_t *woken)
{
   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      retro_u128_t next;
      retro_u128_t chosen;
      uint64_t waiters;
      uint64_t left;

      chosen.lo = 0;
      chosen.hi = 0;

      if (cq_is_count(w))
      {
         /* No waiters: the count grows, to the most */
         if (count > max || w.lo > max - count)
            return LV2_COUNTQ_BUSY;
         next = cq_count(w.lo + count);
      }
      else
      {
         waiters = cq_popcount(w.lo) + cq_popcount(w.hi);

         /* As LV2 has it: the waiters' count is below zero, so as many
          * more may be posted as there are waiters */
         if (count > waiters + max)
            return LV2_COUNTQ_BUSY;

         /* The best waiters, as many as there are counts for */
         next = w;
         for (left = count; left && (next.lo || next.hi); left--)
         {
            const int best = cq_best(q, next, rank, ctx);
            next   = cq_with(next, (unsigned)best, 0);
            chosen = cq_with(chosen, (unsigned)best, 1);
         }

         /* What is left over is a count: no waiter is left then */
         if (left)
            next = cq_count(left);
      }

      if (retro_atomic_cas_128(&q->word, w, next))
      {
         *woken = chosen;
         return LV2_COUNTQ_POSTED;
      }
   }
}

uint64_t lv2_countq_value(lv2_countq_t *q)
{
   const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
   return cq_is_count(w) ? w.lo : 0;
}

int lv2_countq_waits(lv2_countq_t *q, unsigned slot)
{
   const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
   return slot < LV2_COUNTQ_SLOTS && !cq_is_count(w) && cq_bit(w, slot);
}

int lv2_countq_any_waiter(lv2_countq_t *q)
{
   const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
   return !cq_is_count(w) && (w.lo || w.hi);
}
