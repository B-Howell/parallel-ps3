/* The TSC's frequency from CPUID (rpcs3/util/tsc_cpuid.c), so that the
 * core need not sleep 50 ms on load to measure it:
 *
 * - leaf 0x15 with its crystal named: the crystal times the ratio
 *   (an Alder Lake: 38.4 MHz x 188 / 2);
 * - leaf 0x15 with no crystal named, or a ratio of 0, or the leaf beyond
 *   the processor's last: none;
 * - under a hypervisor stating it in leaf 0x40000010: that, before leaf
 *   0x15; a leaf 0x40000000 out of the hypervisor range, or the
 *   hypervisor bit clear (leaf 0x40000000 then repeats a basic leaf):
 *   not used. */

#include "../../util/tsc_cpuid.h"
#include "test_common.h"

#include <string.h>

static uint64_t freq(unsigned max_leaf, unsigned hv, unsigned hv_max, unsigned hv_khz, unsigned den, unsigned num, unsigned crystal)
{
   tsc_cpuid_leaves_t l;
   memset(&l, 0, sizeof(l));
   l.max_leaf    = max_leaf;
   l.hypervisor  = hv;
   l.hv_max_leaf = hv_max;
   l.hv_tsc_khz  = hv_khz;
   l.den         = den;
   l.num         = num;
   l.crystal_hz  = crystal;
   return tsc_cpuid_freq(&l);
}

int main(void)
{
   TEST_CHECK(freq(0x20, 0, 0, 0, 2, 188, 38400000u) == (uint64_t)3609600u * 1000u);
   TEST_CHECK(freq(0x16, 0, 0, 0, 2, 200, 24000000u) == (uint64_t)2400000u * 1000u);
   TEST_CHECK(freq(0x16, 0, 0, 0, 2, 176, 0) == 0);
   TEST_CHECK(freq(0x16, 0, 0, 0, 0, 176, 24000000u) == 0);
   TEST_CHECK(freq(0x16, 0, 0, 0, 2, 0, 24000000u) == 0);
   TEST_CHECK(freq(0x14, 0, 0, 0, 2, 176, 24000000u) == 0);

   TEST_CHECK(freq(0x20, 1, 0x40000010u, 2100000u, 2, 188, 38400000u) == (uint64_t)2100000u * 1000u);
   TEST_CHECK(freq(0x20, 1, 0x40000001u, 2100000u, 0, 0, 0) == 0);
   TEST_CHECK(freq(0x20, 1, 0x40000001u, 0, 2, 188, 38400000u) == (uint64_t)3609600u * 1000u);
   TEST_CHECK(freq(0x20, 1, 0x50000010u, 2100000u, 0, 0, 0) == 0);
   TEST_CHECK(freq(0x20, 0, 0x40000010u, 2100000u, 0, 0, 0) == 0);

   printf("test_tsc_cpuid: the TSC's frequency read from CPUID where stated: crystal ratio, hypervisor leaf first, none otherwise; %s\n", test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
