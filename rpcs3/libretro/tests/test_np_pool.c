/* The NP libraries' memory pool (np_pool.c): first fit, 4-byte granules,
 * frees and shrinks give the right granules back, the stats add up, a
 * save state's allocations are walked and marked back, and threads
 * allocating and freeing at once (NP_POOL_TEST_THREADS, the
 * ThreadSanitizer build) never hand out the same granule twice. C89. */

#include <stdlib.h>
#include <string.h>

#include "../../Emu/NP/np_pool.h"
#include "test_common.h"

#ifdef NP_POOL_TEST_THREADS
#include <rthreads/rthreads.h>
#endif

static void basics(void)
{
   np_pool_t pool;
   uint32_t a, b, c, d, off, size;

   TEST_CHECK(np_pool_init(&pool, 1024));
   TEST_CHECK(np_pool_size(&pool) == 1024 && np_pool_avail(&pool) == 1024);

   TEST_CHECK(!np_pool_alloc(&pool, 0, &a));
   TEST_CHECK(np_pool_alloc(&pool, 10, &a) && a == 0);   /* 12 bytes */
   TEST_CHECK(np_pool_alloc(&pool, 4, &b) && b == 12);
   TEST_CHECK(np_pool_alloc(&pool, 300, &c) && c == 16);
   TEST_CHECK(np_pool_avail(&pool) == 1024 - 12 - 4 - 300);
   TEST_CHECK(np_pool_size_of(&pool, c) == 300);
   TEST_CHECK(np_pool_size_of(&pool, c + 4) == 0);

   /* First fit: a's place goes to what fits in it */
   TEST_CHECK(np_pool_free(&pool, a));
   TEST_CHECK(!np_pool_free(&pool, a));
   TEST_CHECK(!np_pool_free(&pool, c + 4));
   TEST_CHECK(np_pool_alloc(&pool, 8, &d) && d == 0);
   TEST_CHECK(np_pool_alloc(&pool, 8, &a) && a == 316);

   /* Shrunk, its tail is free again and taken next */
   TEST_CHECK(np_pool_shrink(&pool, c, 100));
   TEST_CHECK(!np_pool_shrink(&pool, c, 200));
   TEST_CHECK(np_pool_size_of(&pool, c) == 100);
   TEST_CHECK(np_pool_alloc(&pool, 200, &off) && off == 116);
   TEST_CHECK(np_pool_free(&pool, off));

   /* Too big for any run */
   TEST_CHECK(!np_pool_alloc(&pool, 1024, &off));
   TEST_CHECK(np_pool_max_used(&pool) == 320);

   /* Walked for a save state, and marked back into a new pool */
   {
      np_pool_t back;
      uint32_t from = 0;
      unsigned count = 0;

      TEST_CHECK(np_pool_init(&back, 1024));
      while (np_pool_next(&pool, from, &off, &size))
      {
         TEST_CHECK(np_pool_mark(&back, off, size));
         from = off + size;
         count++;
      }
      TEST_CHECK(count == 4);
      TEST_CHECK(np_pool_avail(&back) == np_pool_avail(&pool));
      TEST_CHECK(!np_pool_mark(&back, c, 4));
      TEST_CHECK(np_pool_alloc(&back, 4, &off) && off == 8);
      np_pool_deinit(&back);
   }

   TEST_CHECK(np_pool_free(&pool, b) && np_pool_free(&pool, c)
         && np_pool_free(&pool, d) && np_pool_free(&pool, a));
   TEST_CHECK(np_pool_avail(&pool) == 1024);
   /* Whole again: all of it in one */
   TEST_CHECK(np_pool_alloc(&pool, 1024, &off) && off == 0);
   np_pool_deinit(&pool);

   /* A size not a multiple of a word of granules: the tail is never handed out */
   TEST_CHECK(np_pool_init(&pool, 4 * 70 + 3));
   TEST_CHECK(np_pool_size(&pool) == 280);
   TEST_CHECK(np_pool_alloc(&pool, 280, &off) && off == 0);
   TEST_CHECK(!np_pool_alloc(&pool, 4, &off));
   np_pool_deinit(&pool);
}

#ifdef NP_POOL_TEST_THREADS
#define THREADS 4
#define HELD 64
#define ROUNDS 20000
#define POOL (64u * 1024u)

static np_pool_t shared;
static unsigned char owner[POOL / 4u];
static retro_atomic_int_t clashes;

typedef struct worker
{
   uint32_t off[HELD];
   uint32_t len[HELD];
   int in_use[HELD];
   unsigned long seed;
   unsigned char id;
} worker_t;

static unsigned wrnd(worker_t *w, unsigned n)
{
   w->seed = w->seed * 1103515245ul + 12345ul;
   return (unsigned)((w->seed >> 8) % n);
}

/* Each granule handed out is stamped with its holder: another's stamp
 * found there means two hold it */
static void stamp(worker_t *w, uint32_t off, uint32_t len, unsigned char who, unsigned char expect)
{
   uint32_t g;
   for (g = off / 4u; g < (off + len + 3u) / 4u; g++)
   {
      if (owner[g] != expect)
         retro_atomic_inc_int(&clashes);
      owner[g] = who;
   }
   (void)w;
}

static void worker_run(void *data)
{
   worker_t *w = (worker_t*)data;
   unsigned r;

   for (r = 0; r < ROUNDS; r++)
   {
      const unsigned i = wrnd(w, HELD);
      if (w->in_use[i])
      {
         stamp(w, w->off[i], w->len[i], 0, w->id);
         if (!np_pool_free(&shared, w->off[i]))
            retro_atomic_inc_int(&clashes);
         w->in_use[i] = 0;
         continue;
      }
      w->len[i] = 1 + wrnd(w, 600);
      if (!np_pool_alloc(&shared, w->len[i], &w->off[i]))
         continue;
      stamp(w, w->off[i], w->len[i], w->id, 0);
      w->in_use[i] = 1;
   }
   for (r = 0; r < HELD; r++)
      if (w->in_use[r])
      {
         stamp(w, w->off[r], w->len[r], 0, w->id);
         if (!np_pool_free(&shared, w->off[r]))
            retro_atomic_inc_int(&clashes);
      }
}

static void threads(void)
{
   worker_t *w = (worker_t*)calloc(THREADS, sizeof(*w));
   sthread_t *t[THREADS];
   unsigned i;
   uint32_t off;

   TEST_CHECK(np_pool_init(&shared, POOL));
   retro_atomic_int_init(&clashes, 0);
   for (i = 0; i < THREADS; i++)
   {
      w[i].seed = 77u + i;
      w[i].id   = (unsigned char)(i + 1);
      t[i]      = sthread_create(worker_run, &w[i]);
   }
   for (i = 0; i < THREADS; i++)
      sthread_join(t[i]);

   TEST_CHECK(retro_atomic_load_acquire_int(&clashes) == 0);
   TEST_CHECK(np_pool_avail(&shared) == POOL);
   TEST_CHECK(np_pool_alloc(&shared, POOL, &off) && off == 0);
   np_pool_deinit(&shared);
   free(w);
   printf("test_np_pool: %d threads allocating and freeing at once, no granule held twice, all of it back\n", THREADS);
}
#endif

int main(void)
{
   basics();
#ifdef NP_POOL_TEST_THREADS
   threads();
#endif
   if (test_failures)
   {
      fprintf(stderr, "test_np_pool: %d failed\n", test_failures);
      return 1;
   }
   printf("test_np_pool: first fit, frees and shrinks exact, stats add up, a save state's allocations walked and marked back\n");
   return 0;
}
