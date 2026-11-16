/* An lv2 lightweight mutex's signal and sleepers, one word (lv2_lwq.h).
 * C89. */

#include "lv2_lwq.h"

#define LQ_ONE       ((uint64_t)1)
#define LQ_HI_SLOTS  ((LQ_ONE << (LV2_LWQ_SLOTS - 64u)) - 1u)
#define LQ_SIGNAL    (LQ_ONE << 62)
#define LQ_SIGNAL2   (LQ_ONE << 63)

static uint32_t lq_signal(retro_u128_t w)
{
   return ((w.hi & LQ_SIGNAL) ? LV2_LWQ_SIGNAL : 0u) | ((w.hi & LQ_SIGNAL2) ? LV2_LWQ_SIGNAL2 : 0u);
}

static retro_u128_t lq_with_signal(retro_u128_t w, uint32_t signal)
{
   w.hi &= ~(LQ_SIGNAL | LQ_SIGNAL2);
   if (signal & LV2_LWQ_SIGNAL)
      w.hi |= LQ_SIGNAL;
   if (signal & LV2_LWQ_SIGNAL2)
      w.hi |= LQ_SIGNAL2;
   return w;
}

static int lq_bit(retro_u128_t w, unsigned slot)
{
   return slot < 64u ? (int)((w.lo >> slot) & 1u) : (int)((w.hi >> (slot - 64u)) & 1u);
}

static retro_u128_t lq_with(retro_u128_t w, unsigned slot, int set)
{
   uint64_t *half   = slot < 64u ? &w.lo : &w.hi;
   const uint64_t m = LQ_ONE << (slot % 64u);
   if (set)
      *half |= m;
   else
      *half &= ~m;
   return w;
}

static int lq_any(retro_u128_t w)
{
   return w.lo || (w.hi & LQ_HI_SLOTS);
}

void lv2_lwq_init(lv2_lwq_t *q, uint32_t signal)
{
   retro_u128_t w;
   unsigned i;

   w.lo = 0;
   w.hi = 0;
   retro_atomic_128_init(&q->word, lq_with_signal(w, signal));
   retro_atomic_int_init(&q->next_ticket, 0);
   for (i = 0; i < LV2_LWQ_SLOTS; i++)
   {
      retro_atomic_int_init(&q->ticket[i], 0);
      retro_atomic_int_init(&q->given2[i], 0);
   }
}

uint32_t lv2_lwq_signal(lv2_lwq_t *q)
{
   return lq_signal(retro_atomic_load_acquire_128(&q->word));
}

uint32_t lv2_lwq_take(lv2_lwq_t *q)
{
   for (;;)
   {
      const retro_u128_t w  = retro_atomic_load_acquire_128(&q->word);
      const uint32_t signal = lq_signal(w);
      if (!signal)
         return 0;
      if (retro_atomic_cas_128(&q->word, w, lq_with_signal(w, 0)))
         return signal;
   }
}

int lv2_lwq_trylock(lv2_lwq_t *q)
{
   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      if (!(lq_signal(w) & LV2_LWQ_SIGNAL))
         return 0;
      if (retro_atomic_cas_128(&q->word, w, lq_with_signal(w, 0)))
         return 1;
   }
}

int lv2_lwq_lock(lv2_lwq_t *q, unsigned slot, uint32_t *signal)
{
   int ticket;

   if (slot >= LV2_LWQ_SLOTS)
      return LV2_LWQ_BAD;

   ticket = retro_atomic_fetch_add_int(&q->next_ticket, 1);

   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      const uint32_t taken = lq_signal(w);

      if (taken)
      {
         if (retro_atomic_cas_128(&q->word, w, lq_with_signal(w, 0)))
         {
            *signal = taken;
            return LV2_LWQ_OWNED;
         }
         continue;
      }

      if (lq_bit(w, slot))
         return LV2_LWQ_BAD;

      /* The ticket before the bit: an unlock that sees the bit reads it */
      retro_atomic_store_release_int(&q->ticket[slot], ticket);
      if (retro_atomic_cas_128(&q->word, w, lq_with(w, slot, 1)))
         return LV2_LWQ_QUEUED;
   }
}

int lv2_lwq_unlock(lv2_lwq_t *q, int unlock2, lv2_lwq_rank_t rank, void *ctx)
{
   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      int best      = -1;
      int best_rank = 0;
      int best_tick = 0;
      unsigned s;

      if (!lq_any(w))
      {
         const uint32_t signal = lq_signal(w) | (unlock2 ? LV2_LWQ_SIGNAL2 : LV2_LWQ_SIGNAL);
         if (retro_atomic_cas_128(&q->word, w, lq_with_signal(w, signal)))
            return -1;
         continue;
      }

      for (s = 0; s < LV2_LWQ_SLOTS; s++)
      {
         int r;
         int t;

         if (!lq_bit(w, s))
            continue;
         r = rank ? rank(ctx, s) : 0;
         t = retro_atomic_load_acquire_int(&q->ticket[s]);
         /* Tickets compared by their difference: they wrap */
         if (best < 0 || r < best_rank
               || (r == best_rank && (int)((unsigned)t - (unsigned)best_tick) < 0))
         {
            best      = (int)s;
            best_rank = r;
            best_tick = t;
         }
      }

      /* How it is given before it is: the sleeper reads it once out of the
       * queue, woken or finding it was given it as it timed out */
      retro_atomic_store_release_int(&q->given2[best], unlock2 ? 1 : 0);
      if (retro_atomic_cas_128(&q->word, w, lq_with(w, (unsigned)best, 0)))
         return best;
   }
}

int lv2_lwq_unlock_free(lv2_lwq_t *q, int unlock2)
{
   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      if (lq_any(w))
         return 0;
      if (retro_atomic_cas_128(&q->word, w,
               lq_with_signal(w, lq_signal(w) | (unlock2 ? LV2_LWQ_SIGNAL2 : LV2_LWQ_SIGNAL))))
         return 1;
   }
}

int lv2_lwq_leave(lv2_lwq_t *q, unsigned slot)
{
   if (slot >= LV2_LWQ_SLOTS)
      return 0;

   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      if (!lq_bit(w, slot))
         return 0;
      if (retro_atomic_cas_128(&q->word, w, lq_with(w, slot, 0)))
         return 1;
   }
}

int lv2_lwq_given2(lv2_lwq_t *q, unsigned slot)
{
   return slot < LV2_LWQ_SLOTS && retro_atomic_load_acquire_int(&q->given2[slot]) != 0;
}

int lv2_lwq_waits(lv2_lwq_t *q, unsigned slot)
{
   return slot < LV2_LWQ_SLOTS && lq_bit(retro_atomic_load_acquire_128(&q->word), slot);
}

int lv2_lwq_any_waiter(lv2_lwq_t *q)
{
   return lq_any(retro_atomic_load_acquire_128(&q->word));
}
