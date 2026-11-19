#include "vm_range_locks.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

/* A slot in use is the rare case: its test is laid out off the loop's
 * straight path, so the common one falls through with no taken branch */
#if defined(__GNUC__)
#define VM_RARE(x) __builtin_expect(!!(x), 0)
#else
#define VM_RARE(x) (x)
#endif

/* A 64-bit index: no sign extension where it scales an address */
static uint64_t lowest_set(uint64_t v)
{
#if defined(_MSC_VER)
   unsigned long i;
   _BitScanForward64(&i, v);
   return i;
#else
   /* Masked to 6 bits: known not negative, so no sign extension where it
    * scales an address */
   return (uint64_t)__builtin_ctzll(v) & 63;
#endif
}

static uint64_t slot_word(vm_range_lock_slot_t *set, uint64_t id)
{
   return (uint64_t)retro_atomic_load_acquire_64(&set[id].word);
}

int vm_range_lock_alloc(retro_atomic_64_t *bits)
{
   for (;;)
   {
      const uint64_t old = (uint64_t)retro_atomic_load_acquire_64(bits);

      /* The top bit is not a slot: with all below it taken, none is free */
      if (!((~(old | (old + 1))) << 1))
         return -1;

      if (retro_atomic_cas_64(bits, (int64_t)old, (int64_t)(old | (old + 1))))
         return (int)lowest_set(~old);
   }
}

void vm_range_lock_free(retro_atomic_64_t *bits, vm_range_lock_slot_t *set, unsigned slot)
{
   slot %= VM_RANGE_LOCKS;
   retro_atomic_store_release_64(&set[slot].word, 0);
   retro_atomic_fetch_and_64(bits, (int64_t)~((uint64_t)1 << slot));
}

uint64_t vm_range_locks_busy_reader(uint64_t mask, vm_range_lock_slot_t *set,
      retro_atomic_64_t *shmem, uint32_t begin, uint32_t size, uint64_t is_share)
{
   /* The hits gathered, not the misses cleared: a slot not in use (the
    * common case) costs no more than its load and test */
   const uint64_t last = (uint64_t)begin + size - 1;
   uint64_t busy       = 0;
   uint64_t bits;

   for (bits = mask; bits; bits &= bits - 1)
   {
      const uint64_t id     = lowest_set(bits);
      const uint64_t value  = slot_word(set, id);
      uint32_t size_exec    = (uint32_t)(value >> 32);
      uint64_t addr_exec    = (uint32_t)value;
      uint64_t addr         = begin;
      uint64_t addr_last    = last;
      uint64_t shared;

      if (VM_RARE(size_exec))
      {
         if ((size_exec & VM_RANGE_FULL_MASK_HI) == VM_RANGE_LOCKED_HI)
         {
            size_exec = 128;

            if (is_share)
            {
               addr      = (uint16_t)addr | is_share;
               addr_last = addr + size - 1;
            }
         }

         size_exec = (size_exec << VM_RANGE_FLAG_BITS) >> VM_RANGE_FLAG_BITS;

         /* TODO (as before): two 64K pages (inverse range), or more */
         shared = (uint64_t)retro_atomic_load_acquire_64(&shmem[addr_exec >> 16]);

         if (shared)
            addr_exec = (uint16_t)addr_exec | shared;

         if (addr <= addr_exec + size_exec - 1 && addr_exec <= addr_last)
            busy |= (uint64_t)1 << id;
      }
   }

   return busy;
}

uint64_t vm_range_locks_busy_overlap(uint64_t mask, vm_range_lock_slot_t *set,
      uint32_t addr, uint32_t size)
{
   const uint32_t end = addr + size - 1;
   uint64_t busy      = 0;
   uint64_t bits;

   for (bits = mask; bits; bits &= bits - 1)
   {
      const uint64_t id    = lowest_set(bits);
      const uint64_t value = slot_word(set, id);
      const uint32_t size2 = (uint32_t)(value >> 32);
      const uint32_t addr2 = (uint32_t)value;

      if (VM_RARE(size2) && addr <= addr2 + size2 - 1 && addr2 <= end)
         busy |= (uint64_t)1 << id;
   }

   return busy;
}

uint64_t vm_range_locks_busy_point(uint64_t mask, vm_range_lock_slot_t *set,
      retro_atomic_64_t *shmem, uint64_t point)
{
   uint64_t busy = 0;
   uint64_t bits;

   for (bits = mask; bits; bits &= bits - 1)
   {
      const uint64_t id    = lowest_set(bits);
      const uint64_t value = slot_word(set, id);
      uint32_t size2       = (uint32_t)(value >> 32);
      uint64_t addr2       = (uint32_t)value;
      uint64_t hi, max;

      if (!VM_RARE(size2) || (size2 >> (61 - 32)) == (VM_RANGE_READABLE_HI >> 29))
         continue;

      /* Split and check every 64K page separately. The size keeps its
       * flag bits here, as before */
      for (hi = addr2 >> 16, max = (addr2 + size2 - 1) >> 16; hi <= max; hi++)
      {
         /* Rounded up, as utils::align did: an address on a page's start
          * gives a size of 0 here, which the test below takes as a hit,
          * as before */
         const uint64_t page_end = (addr2 + 0xffff) & ~(uint64_t)0xffff;
         const uint64_t lim      = addr2 + size2;
         const uint64_t size3    = (lim < page_end ? lim : page_end) - addr2;
         /* The page wraps at 4 GiB as the address does: the C++ code read
          * one past the table for a range crossing the top */
         const uint64_t shared   = (uint64_t)retro_atomic_load_acquire_64(&shmem[hi & 0xffff]);
         uint64_t addr3          = addr2;

         if (shared)
            addr3 = (uint16_t)addr2 | shared;

         if (point - (addr3 / 128) <= (addr3 + size3 - 1) / 128 - (addr3 / 128))
         {
            busy |= (uint64_t)1 << id;
            break;
         }

         addr2 += size3;
         size2 -= (uint32_t)size3;
      }
   }

   return busy;
}
