#ifndef TSC_CPUID_H__
#define TSC_CPUID_H__

/* The TSC's frequency as the processor states it in CPUID, where it
 * states one, so that it need not be measured: measuring it slept the
 * loading thread 50 ms between two reads of the wall clock.
 *
 * - Under a hypervisor (leaf 1, ECX bit 31), leaf 0x40000010 when the
 *   hypervisor has it (VMware; KVM where enabled): EAX the TSC in kHz.
 *   It comes first, as a guest's TSC may be scaled from the host's.
 * - Leaf 0x15 (Intel, from Goldmont and Skylake on): EBX/EAX the TSC's
 *   ratio to the crystal, ECX the crystal in Hz, 0 where not named.
 *   Unnamed, as Linux takes it (native_calibrate_tsc): 25 MHz on
 *   Denverton (family 6, model 0x5F), else from leaf 0x16's base
 *   frequency in MHz (Skylake and Kaby Lake desktop parts). In kHz, as
 *   Linux works it out. Not on other vendors, as Linux.
 *
 * The caller reads the leaves; 0 where none of them states it. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tsc_cpuid_leaves
{
   unsigned max_leaf;      /* leaf 0, EAX */
   unsigned intel;         /* leaf 0's vendor GenuineIntel */
   unsigned family;        /* leaf 1's display family */
   unsigned model;         /* leaf 1's display model */
   unsigned base_mhz;      /* leaf 0x16, EAX bits 0-15 (read where max_leaf allows) */
   unsigned hypervisor;    /* leaf 1, ECX bit 31 */
   unsigned hv_max_leaf;   /* leaf 0x40000000, EAX (read only under a hypervisor) */
   unsigned hv_tsc_khz;    /* leaf 0x40000010, EAX (read only where hv_max_leaf allows) */
   unsigned den;           /* leaf 0x15, EAX */
   unsigned num;           /* leaf 0x15, EBX */
   unsigned crystal_hz;    /* leaf 0x15, ECX */
} tsc_cpuid_leaves_t;

uint64_t tsc_cpuid_freq(const tsc_cpuid_leaves_t *l);

#ifdef __cplusplus
}
#endif

#endif
