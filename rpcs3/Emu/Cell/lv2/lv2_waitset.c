/* The threads waiting on an lv2 sync object, lock-free (lv2_waitset.h).
 * C89. */

#include "lv2_waitset.h"

#define WS_ONE ((uint64_t)1)

static int ws_bit(retro_u128_t v, unsigned slot)
{
   return slot < 64u ? (int)((v.lo >> slot) & 1u) : (int)((v.hi >> (slot - 64u)) & 1u);
}

static retro_u128_t ws_with(retro_u128_t v, unsigned slot, int set)
{
   uint64_t *half   = slot < 64u ? &v.lo : &v.hi;
   const uint64_t m = WS_ONE << (slot % 64u);
   if (set)
      *half |= m;
   else
      *half &= ~m;
   return v;
}

void lv2_waitset_init(lv2_waitset_t *ws)
{
   retro_u128_t zero;
   unsigned i;

   zero.lo = 0;
   zero.hi = 0;
   retro_atomic_128_init(&ws->waiting, zero);
   retro_atomic_int_init(&ws->next_ticket, 0);
   for (i = 0; i < LV2_WAITSET_SLOTS; i++)
      retro_atomic_int_init(&ws->ticket[i], 0);
}

void lv2_waitset_fence(void)
{
   retro_atomic_thread_fence_seq_cst();
}

int lv2_waitset_join(lv2_waitset_t *ws, unsigned slot)
{
   if (slot >= LV2_WAITSET_SLOTS)
      return 0;

   {
      const int ticket = retro_atomic_fetch_add_int(&ws->next_ticket, 1);

      for (;;)
      {
         const retro_u128_t v = retro_atomic_load_acquire_128(&ws->waiting);
         if (ws_bit(v, slot))
            return 0;
         /* The ticket before the bit: a waker that sees the bit reads it */
         retro_atomic_store_release_int(&ws->ticket[slot], ticket);
         if (retro_atomic_cas_128(&ws->waiting, v, ws_with(v, slot, 1)))
            break;
      }
   }

   retro_atomic_thread_fence_seq_cst();
   return 1;
}

int lv2_waitset_rejoin(lv2_waitset_t *ws, unsigned slot)
{
   if (slot >= LV2_WAITSET_SLOTS)
      return 0;

   for (;;)
   {
      const retro_u128_t v = retro_atomic_load_acquire_128(&ws->waiting);
      if (ws_bit(v, slot))
         return 0;
      if (retro_atomic_cas_128(&ws->waiting, v, ws_with(v, slot, 1)))
         break;
   }

   retro_atomic_thread_fence_seq_cst();
   return 1;
}

int lv2_waitset_leave(lv2_waitset_t *ws, unsigned slot)
{
   if (slot >= LV2_WAITSET_SLOTS)
      return 0;

   for (;;)
   {
      const retro_u128_t v = retro_atomic_load_acquire_128(&ws->waiting);
      if (!ws_bit(v, slot))
         return 0;
      if (retro_atomic_cas_128(&ws->waiting, v, ws_with(v, slot, 0)))
         return 1;
   }
}

int lv2_waitset_claim(lv2_waitset_t *ws, lv2_waitset_rank_t rank, void *ctx)
{
   for (;;)
   {
      const retro_u128_t v = retro_atomic_load_acquire_128(&ws->waiting);
      int best       = -1;
      int best_rank  = 0;
      int best_tick  = 0;
      unsigned slot;

      if (!v.lo && !v.hi)
         return -1;

      for (slot = 0; slot < LV2_WAITSET_SLOTS; slot++)
      {
         int r;
         int t;

         if (!ws_bit(v, slot))
            continue;

         r = rank ? rank(ctx, slot) : 0;
         t = retro_atomic_load_acquire_int(&ws->ticket[slot]);

         /* Tickets compared by their difference: they wrap */
         if (best < 0 || r < best_rank
               || (r == best_rank && (int)((unsigned)t - (unsigned)best_tick) < 0))
         {
            best      = (int)slot;
            best_rank = r;
            best_tick = t;
         }
      }

      /* Claimed if it still waits and nothing changed meanwhile; else the
       * waiters are looked at again */
      if (retro_atomic_cas_128(&ws->waiting, v, ws_with(v, (unsigned)best, 0)))
         return best;
   }
}

retro_u128_t lv2_waitset_claim_all(lv2_waitset_t *ws)
{
   retro_u128_t zero;
   zero.lo = 0;
   zero.hi = 0;
   return retro_atomic_exchange_128(&ws->waiting, zero);
}

int lv2_waitset_has(lv2_waitset_t *ws, unsigned slot)
{
   return slot < LV2_WAITSET_SLOTS && ws_bit(retro_atomic_load_acquire_128(&ws->waiting), slot);
}

int lv2_waitset_any(lv2_waitset_t *ws)
{
   const retro_u128_t v = retro_atomic_load_acquire_128(&ws->waiting);
   return v.lo || v.hi;
}
