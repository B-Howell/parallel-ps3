/* An lv2 event queue's events, lock-free (lv2_event_ring.h). C89. */

#include <stdlib.h>
#include <string.h>

#include "lv2_event_ring.h"

int lv2_event_ring_init(lv2_event_ring_t *r, int size)
{
   uint64_t cells = 1;
   uint64_t i;

   memset(r, 0, sizeof(*r));
   if (size < 1)
      size = 1;
   while (cells < (uint64_t)size)
      cells <<= 1;

   r->cells = (lv2_event_cell_t*)calloc((size_t)cells, sizeof(*r->cells));
   if (!r->cells)
      return 0;
   r->mask = cells - 1u;
   r->size = size;
   for (i = 0; i < cells; i++)
      retro_atomic_64_init(&r->cells[i].seq, (int64_t)i);
   retro_atomic_int_init(&r->count, 0);
   retro_atomic_64_init(&r->head, 0);
   retro_atomic_64_init(&r->tail, 0);
   return 1;
}

void lv2_event_ring_deinit(lv2_event_ring_t *r)
{
   free(r->cells);
   memset(r, 0, sizeof(*r));
}

int lv2_event_ring_count(lv2_event_ring_t *r)
{
   return retro_atomic_load_acquire_int(&r->count);
}

int lv2_event_ring_push(lv2_event_ring_t *r, const uint64_t ev[4])
{
   uint64_t pos;
   lv2_event_cell_t *cell;

   /* Room in the queue's own size first */
   if (retro_atomic_fetch_add_int(&r->count, 1) >= r->size)
   {
      (void)retro_atomic_fetch_sub_int(&r->count, 1);
      return 0;
   }

   /* A cell is free for it: the count kept the events under the cells */
   for (;;)
   {
      pos  = (uint64_t)retro_atomic_load_acquire_64(&r->tail);
      cell = &r->cells[pos & r->mask];
      if ((uint64_t)retro_atomic_load_acquire_64(&cell->seq) == pos
            && retro_atomic_cas_64(&r->tail, (int64_t)pos, (int64_t)(pos + 1u)))
         break;
   }

   memcpy(cell->data, ev, sizeof(cell->data));
   retro_atomic_store_release_64(&cell->seq, (int64_t)(pos + 1u));
   return 1;
}

int lv2_event_ring_pop(lv2_event_ring_t *r, uint64_t ev[4])
{
   for (;;)
   {
      const uint64_t pos     = (uint64_t)retro_atomic_load_acquire_64(&r->head);
      lv2_event_cell_t *cell = &r->cells[pos & r->mask];
      const uint64_t seq     = (uint64_t)retro_atomic_load_acquire_64(&cell->seq);

      if (seq != pos + 1u)
      {
         /* Not published yet, or nothing there */
         if ((int64_t)(seq - (pos + 1u)) < 0)
            return 0;
         continue; /* another receiver took it: look again */
      }

      if (!retro_atomic_cas_64(&r->head, (int64_t)pos, (int64_t)(pos + 1u)))
         continue;

      memcpy(ev, cell->data, sizeof(cell->data));
      retro_atomic_store_release_64(&cell->seq, (int64_t)(pos + r->mask + 1u));
      (void)retro_atomic_fetch_sub_int(&r->count, 1);
      return 1;
   }
}

void lv2_event_ring_each(lv2_event_ring_t *r, lv2_event_ring_each_t fn, void *ctx)
{
   uint64_t pos        = (uint64_t)retro_atomic_load_acquire_64(&r->head);
   const uint64_t tail = (uint64_t)retro_atomic_load_acquire_64(&r->tail);

   for (; pos != tail; pos++)
   {
      lv2_event_cell_t *cell = &r->cells[pos & r->mask];
      if ((uint64_t)retro_atomic_load_acquire_64(&cell->seq) == pos + 1u)
         fn(ctx, cell->data);
   }
}
