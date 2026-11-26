/* A short list of {32-bit key, pointer's room} pairs (pair_list.h). C89 */

#include "pair_list.h"

#include <stdlib.h>
#include <string.h>

/* Found by selects, not left early on: where in the list the key lies, or
 * whether it is there, need not be guessed */
pair_list_entry_t *pair_list_find(pair_list_t *l, uint32_t key)
{
   pair_list_entry_t *e;
   uint32_t           i, at;

   if (!l->heap)
   {
      /* Both inline slots compared; one past count may hold an old key, so
       * the count masks it out */
      const uint32_t     n     = l->count;
      const uint32_t     m0    = (l->inline_e[0].key == key) & (n > 0);
      const uint32_t     m1    = (l->inline_e[1].key == key) & (n > 1);
      pair_list_entry_t *found = NULL;

      found = m1 ? &l->inline_e[1] : found;
      found = m0 ? &l->inline_e[0] : found;
      return found;
   }

   e  = l->heap;
   at = l->count;
   for (i = 0; i < l->count; i++)
      at = e[i].key == key ? i : at;
   return at < l->count ? &e[at] : NULL;
}

pair_list_entry_t *pair_list_add(pair_list_t *l, uint32_t key)
{
   pair_list_entry_t *e;

   if (l->count == (l->heap ? l->cap : PAIR_LIST_INLINE))
   {
      const uint32_t     cap  = l->count * 2;
      pair_list_entry_t *heap = (pair_list_entry_t*)malloc(cap * sizeof(*heap));

      if (!heap)
         return NULL;
      memcpy(heap, PAIR_LIST_ENTRIES(l), l->count * sizeof(*heap));
      free(l->heap);
      l->heap = heap;
      l->cap  = cap;
   }

   e      = &PAIR_LIST_ENTRIES(l)[l->count++];
   e->key = key;
   memset(&e->room, 0, sizeof(e->room));
   return e;
}

void pair_list_remove_at(pair_list_t *l, uint32_t index)
{
   pair_list_entry_t *e = PAIR_LIST_ENTRIES(l);

   l->count--;
   if (index != l->count)
      e[index] = e[l->count];
}

void pair_list_free(pair_list_t *l)
{
   free(l->heap);
   memset(l, 0, sizeof(*l));
}
