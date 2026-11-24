/* The TSC's frequency from a perf event's page (rpcs3/util/tsc_perf.c),
 * so that the core need not sleep 50 ms on load to measure it where CPUID
 * does not state it, as on AMD:
 *
 * - from the kernel's conversion, ns = ticks * mult >> shift: 2.1 GHz and
 *   3.7 GHz at a shift of 31, 4.012345 GHz at 30 (to the hertz, rounded),
 *   1 GHz at 10, and the widest shift that fits 64 bits;
 * - a mult of 0, or a shift too wide: none;
 * - read here: none, or between 100 MHz and 10 GHz (cpuinfo's MHz is
 *   the clock now, not the TSC's). The kernel fills the page in only off
 *   a VM (this sandbox is one). */

#include "../../util/tsc_perf.h"
#include "test_common.h"

int main(void)
{
   uint64_t live;

   TEST_CHECK(tsc_perf_freq_from(1022611261u, 31) == (uint64_t)2100000u * 1000u);
   TEST_CHECK(tsc_perf_freq_from(580400986u, 31) == (uint64_t)3700000u * 1000u);
   TEST_CHECK(tsc_perf_freq_from(267609546u, 30) == (uint64_t)4012345u * 1000u + 1u);
   TEST_CHECK(tsc_perf_freq_from(1024u, 10) == 1000000000u);
   TEST_CHECK(tsc_perf_freq_from(4294967295u, 34) == (uint64_t)4000000u * 1000u + 1u);
   TEST_CHECK(tsc_perf_freq_from(0, 31) == 0);
   TEST_CHECK(tsc_perf_freq_from(1022611261u, 35) == 0);

   live = tsc_perf_freq();
   if (live)
      TEST_CHECK(live > (uint64_t)100000u * 1000u && live < (uint64_t)10000000u * 1000u);

   printf("test_tsc_perf: the TSC's frequency from the kernel's conversion: exact, none for a bad mult or shift; read here: %s; %s\n",
         live ? "given" : "not given (no page, or not filled in)", test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
