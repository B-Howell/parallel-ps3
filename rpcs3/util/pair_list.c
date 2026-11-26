/* A short list of {32-bit key, pointer's room} pairs (pair_list.h). C89 */

#include "pair_list.h"

#include <stdlib.h>
#include <string.h>

#include <compat/intrinsics.h>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86_FP) && _M_IX86_FP >= 2
#include <emmintrin.h>
#define PAIR_LIST_SSE2
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define PAIR_LIST_NEON
#endif

/* A bit for each of the four keys from @keys equal to @key */
static uint32_t match4(const uint32_t *keys, uint32_t key)
{
#if defined(PAIR_LIST_SSE2)
   const __m128i k = _mm_set1_epi32((int)key);
   const __m128i v = _mm_loadu_si128((const __m128i*)keys);
   return (uint32_t)_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(v, k)));
#elif defined(PAIR_LIST_NEON)
   static const uint32_t bits[4] = { 1, 2, 4, 8 };
   const uint32x4_t eq = vceqq_u32(vld1q_u32(keys), vdupq_n_u32(key));
   return vaddvq_u32(vandq_u32(eq, vld1q_u32(bits)));
#else
   return (uint32_t)(keys[0] == key) | (uint32_t)(keys[1] == key) << 1
        | (uint32_t)(keys[2] == key) << 2 | (uint32_t)(keys[3] == key) << 3;
#endif
}

uint32_t pair_list_find(const pair_list_t *l, uint32_t key)
{
   const uint32_t n = l->count;
   uint32_t       base;

   if (!l->keys)
   {
      /* Both inline keys compared into a mask; one past count may be an old
       * key, so the count masks it out. Then a select, as below */
      const uint32_t mask = ((uint32_t)(l->inline_keys[0] == key) | (uint32_t)(l->inline_keys[1] == key) << 1)
                          & (((uint32_t)1 << n) - 1);
      return mask ? (mask & 1 ? 0 : 1) : PAIR_LIST_NONE;
   }

   /* Eight or fewer, the common case past the inline two: the heap holds at
    * least eight, so both groups of four are its own; one mask, then a
    * select */
   if (n <= 8)
   {
      const uint32_t mask = (match4(l->keys, key) | match4(l->keys + 4, key) << 4) & (((uint32_t)1 << n) - 1);
      return mask ? (uint32_t)compat_ctz(mask) : PAIR_LIST_NONE;
   }

   if (n <= 16)
   {
      const uint32_t *k    = l->keys;
      const uint32_t  mask = (match4(k, key) | match4(k + 4, key) << 4 | match4(k + 8, key) << 8 | match4(k + 12, key) << 12)
                           & (((uint32_t)1 << n) - 1);
      return mask ? (uint32_t)compat_ctz(mask) : PAIR_LIST_NONE;
   }

   /* More: sixteen a pass (past eight the heap holds a multiple of sixteen,
    * so each pass reads only its own), four at a time into one mask, the
    * ones past count masked off, and only then a branch */
   for (base = 0; base < n; base += 16)
   {
      const uint32_t *k    = l->keys + base;
      uint32_t        mask = match4(k, key) | match4(k + 4, key) << 4 | match4(k + 8, key) << 8 | match4(k + 12, key) << 12;

      if (n - base < 16)
         mask &= ((uint32_t)1 << (n - base)) - 1;
      if (mask)
         return base + (uint32_t)compat_ctz(mask);
   }
   return PAIR_LIST_NONE;
}

uint32_t pair_list_add(pair_list_t *l, uint32_t key)
{
   uint32_t at;

   if (l->count == (l->keys ? l->cap : PAIR_LIST_INLINE))
   {
      const uint32_t    cap   = l->keys ? l->cap * 2 : 8;
      uint32_t         *keys  = (uint32_t*)malloc(cap * (sizeof(uint32_t) + sizeof(pair_list_room_t)));
      pair_list_room_t *rooms = (pair_list_room_t*)(keys + cap);

      if (!keys)
         return PAIR_LIST_NONE;
      memcpy(keys, PAIR_LIST_KEYS(l), l->count * sizeof(*keys));
      memcpy(rooms, PAIR_LIST_ROOMS(l), l->count * sizeof(*rooms));
      free(l->keys);
      l->keys  = keys;
      l->rooms = rooms;
      l->cap   = cap;
   }

   at = l->count++;
   PAIR_LIST_KEYS(l)[at] = key;
   memset(&PAIR_LIST_ROOMS(l)[at], 0, sizeof(pair_list_room_t));
   return at;
}

void pair_list_remove_at(pair_list_t *l, uint32_t index)
{
   l->count--;
   if (index != l->count)
   {
      PAIR_LIST_KEYS(l)[index]  = PAIR_LIST_KEYS(l)[l->count];
      PAIR_LIST_ROOMS(l)[index] = PAIR_LIST_ROOMS(l)[l->count];
   }
}

void pair_list_free(pair_list_t *l)
{
   free(l->keys);
   memset(l, 0, sizeof(*l));
}
