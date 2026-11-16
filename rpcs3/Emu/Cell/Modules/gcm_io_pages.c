/* The IO pages cellGcmSys is mapping or unmapping (gcm_io_pages.h). C89. */

#include "gcm_io_pages.h"

#ifdef GCM_IO_TEST_WINDOW
void GCM_IO_TEST_WINDOW(void);
#endif

/* The bits of pages [io, end) in word @w */
static uint64_t gcm_io_bits(uint32_t w, uint32_t io, uint32_t end)
{
   const uint32_t lo = io > w * 64u ? io : w * 64u;
   const uint32_t hi = end < w * 64u + 64u ? end : w * 64u + 64u;
   uint32_t n;

   if (lo >= hi)
      return 0;
   n = hi - lo;
   return (n == 64u ? ~(uint64_t)0 : (((uint64_t)1 << n) - 1u)) << (lo % 64u);
}

int gcm_io_take(retro_atomic_64_t *pages, uint32_t io, uint32_t count)
{
   const uint32_t end = io + count < GCM_IO_PAGES ? io + count : GCM_IO_PAGES;
   uint32_t w;

   for (w = io / 64u; w * 64u < end; w++)
   {
      const uint64_t bits = gcm_io_bits(w, io, end);

      for (;;)
      {
         const int64_t seen = retro_atomic_load_acquire_64(&pages[w]);

         if ((uint64_t)seen & bits)
         {
            uint32_t back;
            for (back = io / 64u; back < w; back++)
               retro_atomic_fetch_and_64(&pages[back], (int64_t)~gcm_io_bits(back, io, end));
            return 0;
         }

#ifdef GCM_IO_TEST_WINDOW
         /* The test widens the window between the look and the mark */
         GCM_IO_TEST_WINDOW();
#endif

         if (retro_atomic_cas_64(&pages[w], seen, (int64_t)((uint64_t)seen | bits)))
            break;
      }
   }

   return 1;
}

void gcm_io_give(retro_atomic_64_t *pages, uint32_t io, uint32_t count)
{
   const uint32_t end = io + count < GCM_IO_PAGES ? io + count : GCM_IO_PAGES;
   uint32_t w;

   for (w = io / 64u; w * 64u < end; w++)
      retro_atomic_fetch_and_64(&pages[w], (int64_t)~gcm_io_bits(w, io, end));
}

int gcm_io_marked(retro_atomic_64_t *pages, uint32_t page)
{
   return page < GCM_IO_PAGES
      && (((uint64_t)retro_atomic_load_acquire_64(&pages[page / 64u]) >> (page % 64u)) & 1u);
}
