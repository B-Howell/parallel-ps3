#include "spu_pc_table.h"

#include <rthreads/retro_waitaddr.h>

#define LANE_COUNT 0x7fu
#define LANE_WAIT  0x80u

static retro_atomic_int_t *word_of(spu_pc_table_t *table, uint32_t slot)
{
   return &table->words[(slot % SPU_PC_SLOTS) >> 2];
}

static unsigned shift_of(uint32_t slot)
{
   return (slot & 3) * 8;
}

unsigned spu_pc_count(spu_pc_table_t *table, uint32_t slot)
{
   return ((unsigned)retro_atomic_load_acquire_int(word_of(table, slot)) >> shift_of(slot)) & LANE_COUNT;
}

void spu_pc_enter(spu_pc_table_t *table, uint32_t slot)
{
   retro_atomic_fetch_add_int(word_of(table, slot), (int)(1u << shift_of(slot)));
}

void spu_pc_leave(spu_pc_table_t *table, uint32_t slot)
{
   retro_atomic_int_t *w = word_of(table, slot);
   const unsigned sh     = shift_of(slot);
   const unsigned old    = (unsigned)retro_atomic_fetch_sub_int(w, (int)(1u << sh));

   /* Someone waits on this word: the flag cleared before the wake, so a
    * waiter that comes after sets it again */
   if ((old >> sh) & LANE_WAIT)
   {
      retro_atomic_fetch_and_int(w, (int)~(LANE_WAIT << sh));
      retro_waitaddr_notify_all(w);
   }
}

int spu_pc_wait_below(spu_pc_table_t *table, uint32_t slot, unsigned max,
      int64_t timeout_us)
{
   retro_atomic_int_t *w = word_of(table, slot);
   const unsigned sh     = shift_of(slot);

   for (;;)
   {
      retro_waitaddr_t list;
      unsigned seen = (unsigned)retro_atomic_load_acquire_int(w);

      if (((seen >> sh) & LANE_COUNT) < max)
         return 1;

      /* Flag a waiter, then sleep only while the word is as flagged: a
       * leave in between changes it */
      if (!((seen >> sh) & LANE_WAIT))
      {
         seen = (unsigned)retro_atomic_fetch_or_int(w, (int)(LANE_WAIT << sh)) | (LANE_WAIT << sh);

         if (((seen >> sh) & LANE_COUNT) < max)
            return 1;
      }

      list.addr = w;
      list.old  = (uint32_t)seen;

      if (!retro_waitaddr_wait(&list, 1, timeout_us))
         return 0;
   }
}
