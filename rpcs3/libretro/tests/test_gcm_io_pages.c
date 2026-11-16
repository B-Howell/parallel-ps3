/* The IO pages cellGcmSys maps and unmaps, marked with no lock
 * (gcm_io_pages.c): a range across words marked whole or not at all, a
 * range overlapping a marked one refused with nothing left behind; and
 * under ThreadSanitizer threads marking overlapping ranges at once - no
 * page ever held by two. C89. */

#include <stdlib.h>
#include <string.h>

#include "../../Emu/Cell/Modules/gcm_io_pages.h"
#include "test_common.h"

#include <rthreads/rthreads.h>
#include <sched.h>

static retro_atomic_64_t pages[GCM_IO_PAGES / 64];

/* A range marked by another thread between a look and a mark, when armed */
static int window_io = -1;

void gcm_io_test_window(void)
{
   if (window_io >= 0)
   {
      const int io = window_io;
      window_io = -1;
      TEST_CHECK(gcm_io_take(pages, (uint32_t)io, 1));
   }
}

static void basics(void)
{
   unsigned i;

   for (i = 0; i < GCM_IO_PAGES / 64; i++)
      retro_atomic_64_init(&pages[i], 0);

   /* Across three words */
   TEST_CHECK(gcm_io_take(pages, 60, 72));
   TEST_CHECK(gcm_io_marked(pages, 60) && gcm_io_marked(pages, 131) && !gcm_io_marked(pages, 59) && !gcm_io_marked(pages, 132));
   /* Overlapping its last page: refused, and its first words given back */
   TEST_CHECK(!gcm_io_take(pages, 0, 61) && !gcm_io_marked(pages, 0));
   TEST_CHECK(!gcm_io_take(pages, 100, 200));
   TEST_CHECK(!gcm_io_marked(pages, 132) && !gcm_io_marked(pages, 299));
   TEST_CHECK(gcm_io_take(pages, 132, 1) && gcm_io_take(pages, 0, 60));
   gcm_io_give(pages, 0, 133);
   for (i = 0; i < GCM_IO_PAGES; i++)
      TEST_CHECK(!gcm_io_marked(pages, i));

   /* Past the end: what is under it */
   TEST_CHECK(gcm_io_take(pages, 500, 100) && gcm_io_marked(pages, 511) && !gcm_io_marked(pages, 512));
   gcm_io_give(pages, 500, 100);

   /* Another thread marks a page of the word between this one's look and
    * its mark: refused, nothing left marked of its own */
   window_io = 70;
   TEST_CHECK(!gcm_io_take(pages, 64, 10));
   TEST_CHECK(gcm_io_marked(pages, 70) && !gcm_io_marked(pages, 64) && !gcm_io_marked(pages, 73));
   gcm_io_give(pages, 70, 1);
}

#define THREADS 4
#define ROUNDS 30000

static retro_atomic_int_t owner[GCM_IO_PAGES];
static retro_atomic_int_t errors;
static retro_atomic_int_t taken;

static void marker(void *data)
{
   const unsigned me = (unsigned)(size_t)data + 1u;
   uint32_t x = me * 2654435761u;
   unsigned r;

   for (r = 0; r < ROUNDS; r++)
   {
      uint32_t io;
      uint32_t count;
      uint32_t p;

      x     = x * 1664525u + 1013904223u;
      io    = (x >> 8) % 160u;
      count = 1u + (x >> 20) % 140u;

      if (!gcm_io_take(pages, io, count))
         continue;

      retro_atomic_inc_int(&taken);
      for (p = io; p < io + count && p < GCM_IO_PAGES; p++)
         if (retro_atomic_exchange_int(&owner[p], (int)me) != 0)
            retro_atomic_inc_int(&errors);
      for (p = io; p < io + count && p < GCM_IO_PAGES; p++)
         if (retro_atomic_exchange_int(&owner[p], 0) != (int)me)
            retro_atomic_inc_int(&errors);
      gcm_io_give(pages, io, count);
   }
}

static void threads(void)
{
   sthread_t *t[THREADS];
   unsigned i;

   for (i = 0; i < GCM_IO_PAGES / 64; i++)
      retro_atomic_64_init(&pages[i], 0);
   for (i = 0; i < GCM_IO_PAGES; i++)
      retro_atomic_int_init(&owner[i], 0);
   retro_atomic_int_init(&errors, 0);
   retro_atomic_int_init(&taken, 0);

   for (i = 0; i < THREADS; i++)
      t[i] = sthread_create(marker, (void*)(size_t)i);
   for (i = 0; i < THREADS; i++)
      sthread_join(t[i]);

   TEST_CHECK(retro_atomic_load_acquire_int(&errors) == 0);
   TEST_CHECK(retro_atomic_load_acquire_int(&taken) > 1000);
   for (i = 0; i < GCM_IO_PAGES; i++)
      TEST_CHECK(!gcm_io_marked(pages, i));
   printf("test_gcm_io_pages: %d threads marking overlapping ranges: %d taken, no page held by two\n",
         THREADS, retro_atomic_load_acquire_int(&taken));
}

int main(void)
{
   basics();
   threads();
   if (test_failures)
   {
      fprintf(stderr, "test_gcm_io_pages: %d failed\n", test_failures);
      return 1;
   }
   printf("test_gcm_io_pages: ranges across words marked whole or refused with nothing left behind\n");
   return 0;
}
