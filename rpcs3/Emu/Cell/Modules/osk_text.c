/* See osk_text.h. C89. */

#include "osk_text.h"

#include <string.h>
#include <rthreads/retro_waitaddr.h>

static void osk_text_free(mpsc_stack_node_t *link)
{
   osk_text_node_t *node = (osk_text_node_t*)link;
   osk_text_t      *t    = node->owner;

   retro_atomic_store_release_int(&node->used, 0);
   retro_atomic_fetch_add_seq_cst_int(&t->freed, 1);
   retro_waitaddr_notify_all(&t->freed);
}

/* A node from the pool, now used: when every one is published or held by
 * a reader, waits for one to come back */
static osk_text_node_t *osk_text_take(osk_text_t *t)
{
   for (;;)
   {
      retro_waitaddr_t on;
      const int seen = retro_atomic_load_seq_cst_int(&t->freed);
      unsigned i;

      for (i = 0; i < OSK_TEXT_NODES; i++)
      {
         if (!retro_atomic_load_acquire_int(&t->nodes[i].used)
               && retro_atomic_cas_int(&t->nodes[i].used, 0, 1))
            return &t->nodes[i];
      }

      on.addr = &t->freed;
      on.old  = (uint32_t)seen;
      retro_waitaddr_wait(&on, 1, -1);
   }
}

void osk_text_init(osk_text_t *t)
{
   unsigned i;

   memset(t, 0, sizeof(*t));
   retro_retire_init(&t->retire);

   for (i = 0; i < OSK_TEXT_NODES; i++)
   {
      t->nodes[i].owner = t;
      retro_atomic_int_init(&t->nodes[i].used, 0);
   }

   retro_atomic_int_init(&t->freed, 0);
   retro_atomic_int_init(&t->nodes[0].used, 1);
   retro_atomic_ptr_init(&t->current, &t->nodes[0]);
}

void osk_text_set(osk_text_t *t, const uint16_t *src, unsigned n)
{
   osk_text_node_t *node = osk_text_take(t);
   osk_text_node_t *old;
   unsigned i = 0;

   if (n > OSK_TEXT_CHARS - 1)
      n = OSK_TEXT_CHARS - 1;

   if (src)
      for (; i < n && src[i]; i++)
         node->text[i] = src[i];

   memset(node->text + i, 0, (OSK_TEXT_CHARS - i) * sizeof(node->text[0]));

   old = (osk_text_node_t*)retro_atomic_exchange_ptr(&t->current, node);
   retro_retire(&t->retire, &old->link, osk_text_free);
}

const uint16_t *osk_text_enter(osk_text_t *t)
{
   retro_retire_enter(&t->retire);
   return ((osk_text_node_t*)retro_atomic_load_acquire_ptr(&t->current))->text;
}

void osk_text_leave(osk_text_t *t)
{
   retro_retire_leave(&t->retire, osk_text_free);
}
