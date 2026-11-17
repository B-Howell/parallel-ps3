/* The RSX's report timestamps (rsx_timestamp.c): the clock when it moved
 * on, 2 past the last in a burst within one tick, strictly rising through
 * a burst far longer than a tick holds, and back on the clock once it
 * catches up - never a wait. C89. */

#include "../../Emu/RSX/Common/rsx_timestamp.h"
#include "test_common.h"

int main(void)
{
   uint64_t last = 0, prev, t;
   unsigned i;

   TEST_CHECK(rsx_timestamp_next(&last, 1000) == 1000);
   TEST_CHECK(rsx_timestamp_next(&last, 1000) == 1002);
   TEST_CHECK(rsx_timestamp_next(&last, 1000) == 1004);
   TEST_CHECK(rsx_timestamp_next(&last, 1012) == 1012);

   /* 1000 reports in one 12 ns tick: rising by 2, ahead of the clock */
   prev = rsx_timestamp_next(&last, 2000);
   for (i = 0; i < 1000; i++)
   {
      t = rsx_timestamp_next(&last, 2000 + (i / 100) * 12);
      if (t <= prev)
         TEST_CHECK(0);
      prev = t;
   }
   TEST_CHECK(prev == 4000);

   /* The clock catches up: on it again */
   TEST_CHECK(rsx_timestamp_next(&last, 3000) == 4002);
   TEST_CHECK(rsx_timestamp_next(&last, 5000) == 5000);

   if (test_failures)
   {
      fprintf(stderr, "test_rsx_timestamp: %d failed\n", test_failures);
      return 1;
   }
   printf("test_rsx_timestamp: strictly rising, the clock when it moves on, a burst ahead of it, no wait\n");
   return 0;
}
