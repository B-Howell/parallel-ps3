/* Pointers by a 64-bit key in an open-addressed table (ptr_table.h). C89 */

#include "ptr_table.h"

#include <stdlib.h>

#define PTR_TABLE_MIN   16
#define PTR_TABLE_SHIFT 60 /* 64 less log2(PTR_TABLE_MIN) */

/* The key's bits mixed (MurmurHash3's finaliser, its first half), the top
 * of them the slot: addresses a fixed stride apart, which a multiply alone
 * left in runs, spread */
static uint32_t home_of(uint64_t key, uint32_t shift)
{
   const uint64_t mul = ((uint64_t)0xFF51AFD7 << 32) | 0xED558CCD;

   key ^= key >> 33;
   return (uint32_t)((key * mul) >> shift);
}

void *ptr_table_find(const ptr_table_t *t, uint64_t key)
{
   uint32_t i;

   if (!t->count)
      return NULL;
   for (i = home_of(key, t->shift); t->vals[i]; i = (i + 1) & (t->cap - 1))
      if (t->keys[i] == key)
         return t->vals[i];
   return NULL;
}

static void put(uint64_t *keys, void **vals, uint32_t cap, uint32_t shift, uint64_t key, void *val)
{
   uint32_t i = home_of(key, shift);

   while (vals[i])
      i = (i + 1) & (cap - 1);
   keys[i] = key;
   vals[i] = val;
}

int ptr_table_add(ptr_table_t *t, uint64_t key, void *val)
{
   if (!val)
      return 0;

   if ((t->count + 1) * 2 > t->cap)
   {
      const uint32_t cap   = t->cap ? t->cap * 2 : PTR_TABLE_MIN;
      const uint32_t shift = t->cap ? t->shift - 1 : PTR_TABLE_SHIFT;
      uint64_t      *keys = (uint64_t*)malloc(cap * sizeof(*keys));
      void         **vals = (void**)calloc(cap, sizeof(*vals));
      uint32_t       i;

      if (!keys || !vals)
      {
         free(keys);
         free(vals);
         return 0;
      }
      for (i = 0; i < t->cap; i++)
         if (t->vals[i])
            put(keys, vals, cap, shift, t->keys[i], t->vals[i]);
      free(t->keys);
      free(t->vals);
      t->keys = keys;
      t->vals = vals;
      t->cap   = cap;
      t->shift = shift;
   }

   put(t->keys, t->vals, t->cap, t->shift, key, val);
   t->count++;
   return 1;
}

void *ptr_table_slot(const ptr_table_t *t, uint32_t index)
{
   return index < t->cap ? t->vals[index] : NULL;
}

void *ptr_table_remove_at(ptr_table_t *t, uint32_t index)
{
   const uint32_t mask = t->cap - 1;
   uint32_t       hole = index;
   uint32_t       i;
   void          *val;

   if (index >= t->cap || !t->vals[index])
      return NULL;
   val = t->vals[index];

   /* Backward shift: an entry after the hole moves into it unless its home
    * lies cyclically after the hole and up to where it sits */
   for (i = (hole + 1) & mask; t->vals[i]; i = (i + 1) & mask)
   {
      const uint32_t home = home_of(t->keys[i], t->shift);

      if (((i - home) & mask) >= ((i - hole) & mask))
      {
         t->keys[hole] = t->keys[i];
         t->vals[hole] = t->vals[i];
         hole          = i;
      }
   }
   t->vals[hole] = NULL;
   t->count--;
   return val;
}

void *ptr_table_remove(ptr_table_t *t, uint64_t key)
{
   uint32_t i;

   if (!t->count)
      return NULL;
   for (i = home_of(key, t->shift); t->vals[i]; i = (i + 1) & (t->cap - 1))
      if (t->keys[i] == key)
         return ptr_table_remove_at(t, i);
   return NULL;
}

void ptr_table_free(ptr_table_t *t)
{
   free(t->keys);
   free(t->vals);
   t->keys  = NULL;
   t->vals  = NULL;
   t->cap   = 0;
   t->count = 0;
   t->shift = 0;
}
