/* Made by the first start, taken down by the last stop (build_word.h). C89 */

#include "build_word.h"

static uint64_t load(retro_atomic_64_t *w)
{
   return (uint64_t)retro_atomic_load_acquire_64(w);
}

static int cas(retro_atomic_64_t *w, uint64_t seen, uint64_t next)
{
   return retro_atomic_cas_64(w, (int64_t)seen, (int64_t)next);
}

int build_word_start(retro_atomic_64_t *w)
{
   uint64_t s = load(w);

   while (!cas(w, s, (s + 1) | BUILD_WORD_BUILDER))
      s = load(w);

   return !(s & BUILD_WORD_BUILDER);
}

int build_word_stop(retro_atomic_64_t *w)
{
   for (;;)
   {
      const uint64_t s = load(w);

      if (!(uint32_t)s)
         return -1;

      if (cas(w, s, (s - 1) | BUILD_WORD_BUILDER))
         return !(s & BUILD_WORD_BUILDER);
   }
}

enum build_word_action build_word_next(retro_atomic_64_t *w)
{
   for (;;)
   {
      const uint64_t s    = load(w);
      const int      want = (uint32_t)s != 0;

      if (want != !!(s & BUILD_WORD_MADE))
         return want ? BUILD_WORD_MAKE : BUILD_WORD_TAKE_DOWN;

      /* Matches: no builder, unless the count changed since the look */
      if (cas(w, s, s & ~BUILD_WORD_BUILDER))
         return BUILD_WORD_NONE;
   }
}

void build_word_done(retro_atomic_64_t *w)
{
   uint64_t s = load(w);

   while (!cas(w, s, s ^ BUILD_WORD_MADE))
      s = load(w);
}

void build_word_restore(retro_atomic_64_t *w, uint32_t count)
{
   retro_atomic_store_release_64(w, (int64_t)((uint64_t)count | (count ? BUILD_WORD_MADE : 0)));
}

uint32_t build_word_count(retro_atomic_64_t *w)
{
   return (uint32_t)load(w);
}
