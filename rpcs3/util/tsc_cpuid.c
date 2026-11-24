/* The TSC's frequency from CPUID (tsc_cpuid.h). C89 */

#include "tsc_cpuid.h"

uint64_t tsc_cpuid_freq(const tsc_cpuid_leaves_t *l)
{
   if (l->hypervisor && l->hv_max_leaf >= 0x40000010u && l->hv_max_leaf < 0x50000000u && l->hv_tsc_khz)
      return (uint64_t)l->hv_tsc_khz * 1000u;

   if (l->intel && l->max_leaf >= 0x15u && l->den && l->num)
   {
      uint64_t crystal_khz = l->crystal_hz / 1000u;

      if (!crystal_khz && l->family == 6 && l->model == 0x5Fu)
         crystal_khz = 25000u;

      if (!crystal_khz && l->max_leaf >= 0x16u && l->base_mhz)
         crystal_khz = (uint64_t)l->base_mhz * 1000u * l->den / l->num;

      if (crystal_khz)
         return crystal_khz * l->num / l->den * 1000u;
   }

   return 0;
}
