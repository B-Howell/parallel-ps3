/* The TSC's frequency from CPUID (tsc_cpuid.h). C89 */

#include "tsc_cpuid.h"

uint64_t tsc_cpuid_freq(const tsc_cpuid_leaves_t *l)
{
   if (l->hypervisor && l->hv_max_leaf >= 0x40000010u && l->hv_max_leaf < 0x50000000u && l->hv_tsc_khz)
      return (uint64_t)l->hv_tsc_khz * 1000u;

   if (l->max_leaf >= 0x15u && l->den && l->num && l->crystal_hz)
      return (uint64_t)l->crystal_hz * l->num / l->den;

   return 0;
}
