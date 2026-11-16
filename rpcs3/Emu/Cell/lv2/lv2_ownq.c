/* An lv2 mutex's owner and waiters, one word (lv2_ownq.h). C89. */

#include "lv2_ownq.h"

#define OQ_ONE         ((uint64_t)1)
#define OQ_OWNER_SHIFT 56u
#define OQ_OWNER_MASK  ((uint64_t)0x7f << OQ_OWNER_SHIFT)
#define OQ_HI_SLOTS    (((OQ_ONE << (LV2_OWNQ_SLOTS - 64u)) - 1u))

static int oq_owner(retro_u128_t w)
{
   return (int)((w.hi & OQ_OWNER_MASK) >> OQ_OWNER_SHIFT) - 1;
}

static retro_u128_t oq_with_owner(retro_u128_t w, int owner)
{
   w.hi = (w.hi & ~OQ_OWNER_MASK) | ((uint64_t)(owner + 1) << OQ_OWNER_SHIFT);
   return w;
}

static int oq_bit(retro_u128_t w, unsigned slot)
{
   return slot < 64u ? (int)((w.lo >> slot) & 1u) : (int)((w.hi >> (slot - 64u)) & 1u);
}

static retro_u128_t oq_with(retro_u128_t w, unsigned slot, int set)
{
   uint64_t *half   = slot < 64u ? &w.lo : &w.hi;
   const uint64_t m = OQ_ONE << (slot % 64u);
   if (set)
      *half |= m;
   else
      *half &= ~m;
   return w;
}

static int oq_any(retro_u128_t w)
{
   return w.lo || (w.hi & OQ_HI_SLOTS);
}

void lv2_ownq_init(lv2_ownq_t *q, int owner)
{
   retro_u128_t w;
   unsigned i;

   w.lo = 0;
   w.hi = 0;
   if (owner >= 0 && (unsigned)owner < LV2_OWNQ_SLOTS)
      w = oq_with_owner(w, owner);
   retro_atomic_128_init(&q->word, w);
   retro_atomic_int_init(&q->next_ticket, 0);
   for (i = 0; i < LV2_OWNQ_SLOTS; i++)
      retro_atomic_int_init(&q->ticket[i], 0);
}

int lv2_ownq_trylock(lv2_ownq_t *q, unsigned slot)
{
   if (slot >= LV2_OWNQ_SLOTS)
      return LV2_OWNQ_BAD;

   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      const int owner      = oq_owner(w);

      if (owner == (int)slot)
         return LV2_OWNQ_MINE;
      if (owner >= 0)
         return LV2_OWNQ_BUSY;
      if (retro_atomic_cas_128(&q->word, w, oq_with_owner(w, (int)slot)))
         return LV2_OWNQ_OWNED;
   }
}

/* Owned when free; else queued. @mine_counts: the caller owning it is MINE
 * (a lock) or queues it like any (a condition's wake) */
static int oq_take_or_queue(lv2_ownq_t *q, unsigned slot, int mine_counts)
{
   int ticket;

   if (slot >= LV2_OWNQ_SLOTS)
      return LV2_OWNQ_BAD;

   ticket = retro_atomic_fetch_add_int(&q->next_ticket, 1);

   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      const int owner      = oq_owner(w);

      if (owner < 0)
      {
         if (retro_atomic_cas_128(&q->word, w, oq_with_owner(w, (int)slot)))
            return LV2_OWNQ_OWNED;
         continue;
      }

      if (owner == (int)slot && mine_counts)
         return LV2_OWNQ_MINE;
      if (oq_bit(w, slot))
         return LV2_OWNQ_BAD;

      /* The ticket before the bit: an unlock that sees the bit reads it */
      retro_atomic_store_release_int(&q->ticket[slot], ticket);
      if (retro_atomic_cas_128(&q->word, w, oq_with(w, slot, 1)))
         return LV2_OWNQ_QUEUED;
   }
}

int lv2_ownq_lock(lv2_ownq_t *q, unsigned slot)
{
   return oq_take_or_queue(q, slot, 1);
}

int lv2_ownq_give(lv2_ownq_t *q, unsigned slot)
{
   return oq_take_or_queue(q, slot, 0);
}

int lv2_ownq_leave(lv2_ownq_t *q, unsigned slot)
{
   if (slot >= LV2_OWNQ_SLOTS)
      return 0;

   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      if (!oq_bit(w, slot))
         return 0;
      if (retro_atomic_cas_128(&q->word, w, oq_with(w, slot, 0)))
         return 1;
   }
}

int lv2_ownq_unlock(lv2_ownq_t *q, unsigned slot, lv2_ownq_rank_t rank, void *ctx)
{
   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      int best      = -1;
      int best_rank = 0;
      int best_tick = 0;
      unsigned s;

      if (oq_owner(w) != (int)slot)
         return LV2_OWNQ_NOT_OWNER;

      if (!oq_any(w))
      {
         if (retro_atomic_cas_128(&q->word, w, oq_with_owner(w, -1)))
            return LV2_OWNQ_FREED;
         continue;
      }

      for (s = 0; s < LV2_OWNQ_SLOTS; s++)
      {
         int r;
         int t;

         if (!oq_bit(w, s))
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

      /* Out of the queue and the owner, in one change */
      if (retro_atomic_cas_128(&q->word, w, oq_with_owner(oq_with(w, (unsigned)best, 0), best)))
         return best;
   }
}

int lv2_ownq_owner(lv2_ownq_t *q)
{
   return oq_owner(retro_atomic_load_acquire_128(&q->word));
}

int lv2_ownq_waits(lv2_ownq_t *q, unsigned slot)
{
   return slot < LV2_OWNQ_SLOTS && oq_bit(retro_atomic_load_acquire_128(&q->word), slot);
}

int lv2_ownq_any_waiter(lv2_ownq_t *q)
{
   return oq_any(retro_atomic_load_acquire_128(&q->word));
}
