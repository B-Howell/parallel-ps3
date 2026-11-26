/* A short list of pointers by a 64-bit key (key_list.h). C89 */

#include "key_list.h"

#include <stdlib.h>
#include <string.h>

static const uint64_t *keys_of(const key_list_t *l) { return l->cap ? l->keys : l->ikeys; }
static void *const *vals_of(const key_list_t *l) { return l->cap ? l->vals : l->ivals; }

void *key_list_find(const key_list_t *l, uint64_t key)
{
   const uint64_t *k = keys_of(l);
   uint32_t        i;

   for (i = 0; i < l->count; i++)
      if (k[i] == key)
         return vals_of(l)[i];
   return NULL;
}

int key_list_add(key_list_t *l, uint64_t key, void *val)
{
   const uint32_t room = l->cap ? l->cap : KEY_LIST_INLINE;

   if (l->count == room)
   {
      const uint32_t cap  = room * 2;
      uint64_t      *keys = (uint64_t*)malloc(cap * sizeof(*keys));
      void         **vals = (void**)malloc(cap * sizeof(*vals));

      if (!keys || !vals)
      {
         free(keys);
         free(vals);
         return 0;
      }
      memcpy(keys, keys_of(l), l->count * sizeof(*keys));
      memcpy(vals, vals_of(l), l->count * sizeof(*vals));
      if (l->cap)
      {
         free(l->keys);
         free(l->vals);
      }
      l->keys = keys;
      l->vals = vals;
      l->cap  = cap;
   }

   if (l->cap)
   {
      l->keys[l->count] = key;
      l->vals[l->count] = val;
   }
   else
   {
      l->ikeys[l->count] = key;
      l->ivals[l->count] = val;
   }
   l->count++;
   return 1;
}

void *key_list_replace(key_list_t *l, uint64_t key, void *val)
{
   const uint64_t *k    = keys_of(l);
   void          **vals = l->cap ? l->vals : l->ivals;
   uint32_t        i;

   for (i = 0; i < l->count; i++)
      if (k[i] == key)
      {
         void *old = vals[i];
         vals[i]   = val;
         return old;
      }
   return NULL;
}

void *key_list_at(const key_list_t *l, uint32_t index)
{
   return index < l->count ? vals_of(l)[index] : NULL;
}

void key_list_free(key_list_t *l)
{
   if (l->cap)
   {
      free(l->keys);
      free(l->vals);
   }
   memset(l, 0, sizeof(*l));
}

void key_list_move(key_list_t *dst, key_list_t *src)
{
   memcpy(dst, src, sizeof(*dst));
   memset(src, 0, sizeof(*src));
}
