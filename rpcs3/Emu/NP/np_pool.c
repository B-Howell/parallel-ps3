/* The NP libraries' memory pool, lock-free (np_pool.h). C89. */

#include <stdlib.h>
#include <string.h>

#include "np_pool.h"

#define NP_POOL_ONE ((uint64_t)1)

static uint64_t np_pool_load(retro_atomic_64_t *w)
{
   return (uint64_t)retro_atomic_load_acquire_64(w);
}

/* The bits of word w that granules lo..hi (inclusive) take */
static uint64_t np_pool_mask(uint32_t w, uint32_t lo, uint32_t hi)
{
   const uint32_t base = w * 64u;
   const uint32_t a    = lo > base ? lo - base : 0;
   const uint32_t b    = hi < base + 63u ? hi - base : 63u;
   uint64_t mask       = (b == 63u) ? ~(uint64_t)0 : ((NP_POOL_ONE << (b + 1u)) - 1u);
   return mask & ~((NP_POOL_ONE << a) - 1u);
}

static int np_pool_bit(retro_atomic_64_t *map, uint32_t g)
{
   return (int)((np_pool_load(&map[g / 64u]) >> (g % 64u)) & 1u);
}

static void np_pool_set(retro_atomic_64_t *map, uint32_t g)
{
   (void)retro_atomic_fetch_or_64(&map[g / 64u], (int64_t)(NP_POOL_ONE << (g % 64u)));
}

static void np_pool_clear(retro_atomic_64_t *map, uint32_t g)
{
   (void)retro_atomic_fetch_and_64(&map[g / 64u], (int64_t)~(NP_POOL_ONE << (g % 64u)));
}

/* Granules lo..hi in use no more */
static void np_pool_release(np_pool_t *pool, uint32_t lo, uint32_t hi)
{
   uint32_t w;
   for (w = lo / 64u; w <= hi / 64u; w++)
      (void)retro_atomic_fetch_and_64(&pool->used[w], (int64_t)~np_pool_mask(w, lo, hi));
}

int np_pool_init(np_pool_t *pool, uint32_t size)
{
   uint32_t i;

   memset(pool, 0, sizeof(*pool));
   pool->granules = size / NP_POOL_GRANULE;
   pool->words    = (pool->granules + 63u) / 64u;
   retro_atomic_int_init(&pool->avail, (int)(pool->granules * NP_POOL_GRANULE));
   retro_atomic_int_init(&pool->max_used, 0);
   if (!pool->words)
      return 1;

   pool->used  = (retro_atomic_64_t*)malloc(3u * pool->words * sizeof(retro_atomic_64_t));
   if (!pool->used)
   {
      pool->granules = 0;
      pool->words    = 0;
      retro_atomic_int_init(&pool->avail, 0);
      return 0;
   }
   pool->first = pool->used + pool->words;
   pool->last  = pool->first + pool->words;
   for (i = 0; i < 3u * pool->words; i++)
      retro_atomic_64_init(&pool->used[i], 0);

   /* The granules past the end of the last word are never free */
   if (pool->granules % 64u)
      retro_atomic_64_init(&pool->used[pool->words - 1u],
            (int64_t)~((NP_POOL_ONE << (pool->granules % 64u)) - 1u));
   return 1;
}

void np_pool_deinit(np_pool_t *pool)
{
   free(pool->used);
   memset(pool, 0, sizeof(*pool));
}

uint32_t np_pool_size(const np_pool_t *pool)
{
   return pool->granules * NP_POOL_GRANULE;
}

uint32_t np_pool_avail(const np_pool_t *pool)
{
   return (uint32_t)retro_atomic_load_acquire_int((retro_atomic_int_t*)&pool->avail);
}

uint32_t np_pool_max_used(const np_pool_t *pool)
{
   return (uint32_t)retro_atomic_load_acquire_int((retro_atomic_int_t*)&pool->max_used);
}

/* The first run of n free granules at from or after, as the words read
 * now show it. granules when there is none */
static uint32_t np_pool_find(np_pool_t *pool, uint32_t from, uint32_t n)
{
   uint32_t run_start = from;
   uint32_t run       = 0;
   uint32_t g         = from;

   while (g < pool->granules)
   {
      const uint64_t word = np_pool_load(&pool->used[g / 64u]);
      const uint32_t bit  = g % 64u;

      /* A whole free word at once */
      if (!bit && !word)
      {
         if (!run)
            run_start = g;
         run += 64u;
         g   += 64u;
      }
      else if ((word >> bit) & 1u)
      {
         run = 0;
         g++;
      }
      else
      {
         if (!run)
            run_start = g;
         run++;
         g++;
      }

      if (run >= n)
         return run_start;
   }
   return pool->granules;
}

static void np_pool_note_use(np_pool_t *pool, uint32_t bytes)
{
   const int left = retro_atomic_fetch_sub_int(&pool->avail, (int)bytes) - (int)bytes;
   const int used = (int)(pool->granules * NP_POOL_GRANULE) - left;
   int seen       = retro_atomic_load_acquire_int(&pool->max_used);

   while (used > seen && !retro_atomic_cas_int(&pool->max_used, seen, used))
      seen = retro_atomic_load_acquire_int(&pool->max_used);
}

int np_pool_alloc(np_pool_t *pool, uint32_t size, uint32_t *offset)
{
   uint32_t n;
   uint32_t from = 0;

   if (!size || !pool->granules)
      return 0;
   n = (size + NP_POOL_GRANULE - 1u) / NP_POOL_GRANULE;
   if (n > pool->granules)
      return 0;

   for (;;)
   {
      const uint32_t s = np_pool_find(pool, from, n);
      const uint32_t e = s + n - 1u;
      uint32_t w;
      uint32_t claimed_to = 0;
      int lost = 0;

      if (s >= pool->granules || e >= pool->granules)
         return 0;

      /* Each word's share of the run, left to right */
      for (w = s / 64u; w <= e / 64u; w++)
      {
         const uint64_t mask = np_pool_mask(w, s, e);
         uint64_t old        = np_pool_load(&pool->used[w]);

         for (;;)
         {
            if (old & mask)
            {
               lost = 1;
               break;
            }
            if (retro_atomic_cas_64(&pool->used[w], (int64_t)old, (int64_t)(old | mask)))
               break;
            old = np_pool_load(&pool->used[w]);
         }
         if (lost)
            break;
         claimed_to = w + 1u;
      }

      if (!lost)
      {
         /* The last granule first: a free looks for it */
         np_pool_set(pool->last, e);
         np_pool_set(pool->first, s);
         np_pool_note_use(pool, n * NP_POOL_GRANULE);
         *offset = s * NP_POOL_GRANULE;
         return 1;
      }

      /* Another thread took part of the run: what was claimed goes back,
       * and the lowest run is looked for again */
      for (w = s / 64u; w < claimed_to; w++)
         (void)retro_atomic_fetch_and_64(&pool->used[w], (int64_t)~np_pool_mask(w, s, e));
      from = 0;
   }
}

/* The allocation's last granule: the first marked at s or after */
static uint32_t np_pool_end_of(const np_pool_t *pool, uint32_t s)
{
   uint32_t w;

   for (w = s / 64u; w < pool->words; w++)
   {
      uint64_t word = (uint64_t)retro_atomic_load_acquire_64(&pool->last[w]);
      if (w == s / 64u)
         word &= ~((NP_POOL_ONE << (s % 64u)) - 1u);
      if (word)
      {
         uint32_t bit = 0;
         while (!((word >> bit) & 1u))
            bit++;
         return w * 64u + bit;
      }
   }
   return pool->granules;
}

int np_pool_free(np_pool_t *pool, uint32_t offset)
{
   const uint32_t s = offset / NP_POOL_GRANULE;
   uint32_t e;

   if (offset % NP_POOL_GRANULE || s >= pool->granules
         || !np_pool_bit(pool->first, s))
      return 0;
   e = np_pool_end_of(pool, s);
   if (e >= pool->granules)
      return 0;

   /* Its marks first, while the granules are still taken: nobody else's
    * can be at the same place yet */
   np_pool_clear(pool->first, s);
   np_pool_clear(pool->last, e);
   np_pool_release(pool, s, e);
   (void)retro_atomic_fetch_add_int(&pool->avail, (int)((e - s + 1u) * NP_POOL_GRANULE));
   return 1;
}

int np_pool_shrink(np_pool_t *pool, uint32_t offset, uint32_t new_size)
{
   const uint32_t s = offset / NP_POOL_GRANULE;
   uint32_t e;
   uint32_t n;

   if (offset % NP_POOL_GRANULE || s >= pool->granules
         || !np_pool_bit(pool->first, s))
      return 0;
   e = np_pool_end_of(pool, s);
   if (e >= pool->granules)
      return 0;

   n = (new_size + NP_POOL_GRANULE - 1u) / NP_POOL_GRANULE;
   if (!n)
      n = 1;
   if (n > e - s + 1u)
      return 0;
   if (n == e - s + 1u)
      return 1;

   /* The new end is marked before the old goes, and the tail is freed
    * last, while nothing else can be there */
   np_pool_set(pool->last, s + n - 1u);
   np_pool_clear(pool->last, e);
   np_pool_release(pool, s + n, e);
   (void)retro_atomic_fetch_add_int(&pool->avail, (int)((e - (s + n) + 1u) * NP_POOL_GRANULE));
   return 1;
}

uint32_t np_pool_size_of(const np_pool_t *pool, uint32_t offset)
{
   const uint32_t s = offset / NP_POOL_GRANULE;
   uint32_t e;

   if (offset % NP_POOL_GRANULE || s >= pool->granules
         || !np_pool_bit(pool->first, s))
      return 0;
   e = np_pool_end_of(pool, s);
   if (e >= pool->granules)
      return 0;
   return (e - s + 1u) * NP_POOL_GRANULE;
}

int np_pool_next(const np_pool_t *pool, uint32_t from, uint32_t *offset,
      uint32_t *size)
{
   uint32_t g;

   for (g = (from + NP_POOL_GRANULE - 1u) / NP_POOL_GRANULE; g < pool->granules; g++)
   {
      if (np_pool_bit(pool->first, g))
      {
         *offset = g * NP_POOL_GRANULE;
         *size   = np_pool_size_of(pool, *offset);
         return 1;
      }
   }
   return 0;
}

int np_pool_mark(np_pool_t *pool, uint32_t offset, uint32_t size)
{
   const uint32_t s = offset / NP_POOL_GRANULE;
   uint32_t n;
   uint32_t e;
   uint32_t g;

   if (offset % NP_POOL_GRANULE || !size)
      return 0;
   n = (size + NP_POOL_GRANULE - 1u) / NP_POOL_GRANULE;
   e = s + n - 1u;
   if (s >= pool->granules || e >= pool->granules || e < s)
      return 0;
   for (g = s; g <= e; g++)
      if (np_pool_bit(pool->used, g))
         return 0;

   for (g = s; g <= e; g++)
      np_pool_set(pool->used, g);
   np_pool_set(pool->last, e);
   np_pool_set(pool->first, s);
   np_pool_note_use(pool, n * NP_POOL_GRANULE);
   return 1;
}
