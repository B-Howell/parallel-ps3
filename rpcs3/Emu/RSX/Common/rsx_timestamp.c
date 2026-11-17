/* The RSX's report timestamps (rsx_timestamp.h) */

#include "rsx_timestamp.h"

uint64_t rsx_timestamp_next(uint64_t *last, uint64_t now)
{
   const uint64_t next = *last + 2;

   if (now < next)
      now = next;

   *last = now;
   return now;
}
