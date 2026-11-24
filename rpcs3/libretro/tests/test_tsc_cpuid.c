/* The TSC's frequency from CPUID (rpcs3/util/tsc_cpuid.c), so that the
 * core need not sleep 50 ms on load to measure it:
 *
 * - leaf 0x15 with its crystal named: the crystal times the ratio
 *   (an Alder Lake: 38.4 MHz x 188 / 2);
 * - leaf 0x15 with no crystal named, as Linux takes it: 25 MHz on
 *   Denverton (family 6, model 0x5F), else from leaf 0x16's base
 *   frequency (an i7-6700K: 4000 MHz at 334 / 2, a 23952 kHz crystal,
 *   3999984 kHz as Linux has it), and none without leaf 0x16;
 * - a ratio of 0, leaf 0x15 beyond the processor's last, or a vendor not
 *   Intel: none;
 * - under a hypervisor stating it in leaf 0x40000010: that, before leaf
 *   0x15; a leaf 0x40000000 out of the hypervisor range, or the
 *   hypervisor bit clear (leaf 0x40000000 then repeats a basic leaf):
 *   not used. */

#include "../../util/tsc_cpuid.h"
#include "test_common.h"

#include <string.h>

static tsc_cpuid_leaves_t l;

static void intel(unsigned max_leaf, unsigned model, unsigned den, unsigned num, unsigned crystal, unsigned base_mhz)
{
   memset(&l, 0, sizeof(l));
   l.intel      = 1;
   l.family     = 6;
   l.model      = model;
   l.max_leaf   = max_leaf;
   l.den        = den;
   l.num        = num;
   l.crystal_hz = crystal;
   l.base_mhz   = base_mhz;
}

#define GHZ(m) ((uint64_t)(m) * 1000000u) /* m in MHz */

int main(void)
{
   intel(0x20, 0x97, 2, 188, 38400000u, 0);
   TEST_CHECK(tsc_cpuid_freq(&l) == GHZ(3609) + 600000u);
   l.intel = 0;
   TEST_CHECK(tsc_cpuid_freq(&l) == 0);

   intel(0x16, 0x5E, 2, 334, 0, 4000);
   TEST_CHECK(tsc_cpuid_freq(&l) == (uint64_t)3999984u * 1000u);
   l.base_mhz = 0;
   TEST_CHECK(tsc_cpuid_freq(&l) == 0);
   intel(0x15, 0x5E, 2, 284, 0, 3400);
   TEST_CHECK(tsc_cpuid_freq(&l) == 0);

   intel(0x15, 0x5F, 1, 88, 0, 0);
   TEST_CHECK(tsc_cpuid_freq(&l) == GHZ(2200));
   l.family = 15;
   TEST_CHECK(tsc_cpuid_freq(&l) == 0);

   intel(0x16, 0x5E, 0, 284, 24000000u, 3400);
   TEST_CHECK(tsc_cpuid_freq(&l) == 0);
   intel(0x16, 0x5E, 2, 0, 24000000u, 3400);
   TEST_CHECK(tsc_cpuid_freq(&l) == 0);
   intel(0x14, 0x5E, 2, 200, 24000000u, 0);
   TEST_CHECK(tsc_cpuid_freq(&l) == 0);

   intel(0x20, 0x97, 2, 188, 38400000u, 0);
   l.hypervisor = 1; l.hv_max_leaf = 0x40000010u; l.hv_tsc_khz = 2100000u;
   TEST_CHECK(tsc_cpuid_freq(&l) == GHZ(2100));
   l.hv_max_leaf = 0x40000001u;
   TEST_CHECK(tsc_cpuid_freq(&l) == GHZ(3609) + 600000u);
   l.hv_max_leaf = 0x50000010u;
   TEST_CHECK(tsc_cpuid_freq(&l) == GHZ(3609) + 600000u);
   l.hv_max_leaf = 0x40000010u; l.hypervisor = 0;
   TEST_CHECK(tsc_cpuid_freq(&l) == GHZ(3609) + 600000u);
   l.hypervisor = 1; l.hv_tsc_khz = 0;
   TEST_CHECK(tsc_cpuid_freq(&l) == GHZ(3609) + 600000u);

   printf("test_tsc_cpuid: the TSC's frequency read from CPUID where stated: crystal ratio, Linux's unnamed crystal, hypervisor leaf first, none otherwise; %s\n", test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
