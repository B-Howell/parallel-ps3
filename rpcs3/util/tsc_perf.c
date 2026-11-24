/* The TSC's frequency from a perf event's page (tsc_perf.h). C89 */

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* syscall */
#endif

#include "tsc_perf.h"

#if defined(__linux__) && !defined(__ANDROID__)
#include <retro_atomic.h>
#include <linux/perf_event.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

uint64_t tsc_perf_freq_from(uint32_t mult, uint32_t shift)
{
   if (!mult || shift > 34)
      return 0;

   return (((uint64_t)1000000000u << shift) + mult / 2) / mult;
}

uint64_t tsc_perf_freq(void)
{
#if defined(__linux__) && !defined(__ANDROID__)
   struct perf_event_attr attr;
   volatile struct perf_event_mmap_page *page;
   uint64_t freq = 0;
   long size     = sysconf(_SC_PAGESIZE);
   void *map;
   int fd, tries;

   memset(&attr, 0, sizeof(attr));
   attr.type           = PERF_TYPE_SOFTWARE;
   attr.size           = sizeof(attr);
   attr.config         = PERF_COUNT_SW_TASK_CLOCK;
   attr.exclude_kernel = 1;
   attr.exclude_hv     = 1;

   fd = (int)syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
   if (fd < 0)
      return 0;

   map = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, fd, 0);
   if (map != MAP_FAILED)
   {
      page = (volatile struct perf_event_mmap_page*)map;

      /* The page's sequence count unchanged across the read: the kernel
       * was not writing it. A few tries, then none */
      for (tries = 0; tries < 8; tries++)
      {
         uint32_t seq = page->lock, mult, shift, cap;
         retro_atomic_thread_fence_seq_cst();
         cap   = (uint32_t)page->cap_user_time;
         mult  = page->time_mult;
         shift = page->time_shift;
         retro_atomic_thread_fence_seq_cst();

         if (page->lock == seq && !(seq & 1))
         {
            if (cap)
               freq = tsc_perf_freq_from(mult, shift);
            break;
         }
      }

      munmap(map, (size_t)size);
   }

   close(fd);
   return freq;
#else
   return 0;
#endif
}
