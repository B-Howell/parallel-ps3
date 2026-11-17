/* The RPCN client's sync replies (rpcn_reply_slots.h) */

#include "rpcn_reply_slots.h"

void rpcn_reply_slots_init(rpcn_reply_slots_t *s)
{
   int i;
   for (i = 0; i < RPCN_REPLY_SLOTS; i++)
   {
      retro_atomic_int_init(&s->state[i], 0);
      retro_atomic_store_relaxed_64(&s->id[i], 0);
   }
}

int rpcn_reply_slots_claim(rpcn_reply_slots_t *s)
{
   int i;
   for (i = 0; i < RPCN_REPLY_SLOTS; i++)
      if (retro_atomic_cas_int(&s->state[i], 0, 1))
         return i;
   return -1;
}

void rpcn_reply_slots_publish(rpcn_reply_slots_t *s, int slot, uint64_t id)
{
   retro_atomic_store_relaxed_64(&s->id[slot], id);
   /* The id and what goes with it, before the slot is seen full */
   retro_atomic_store_release_int(&s->state[slot], 2);
}

int rpcn_reply_slots_take(rpcn_reply_slots_t *s, uint64_t id)
{
   int i;
   for (i = 0; i < RPCN_REPLY_SLOTS; i++)
   {
      /* Full with @id: the compare-and-swap takes it only if it is still
       * full - and the id read is then the one published with it, as a
       * slot can be full only once between two frees */
      if (retro_atomic_load_acquire_int(&s->state[i]) == 2
            && (uint64_t)retro_atomic_load_relaxed_64(&s->id[i]) == id
            && retro_atomic_cas_int(&s->state[i], 2, 3))
         return i;
   }
   return -1;
}

void rpcn_reply_slots_free(rpcn_reply_slots_t *s, int slot)
{
   retro_atomic_store_release_int(&s->state[slot], 0);
}

int rpcn_reply_slots_drop(rpcn_reply_slots_t *s, int from)
{
   int i;
   for (i = from; i < RPCN_REPLY_SLOTS; i++)
      if (retro_atomic_cas_int(&s->state[i], 2, 3))
         return i;
   return -1;
}
