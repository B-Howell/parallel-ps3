#include "jit_bump.h"

static uint64_t align_up(uint64_t v, uint64_t align)
{
   return (v + (align - 1)) & (0 - align);
}

uint64_t jit_bump_alloc(retro_atomic_64_t *ctr, uint64_t size, uint64_t align,
      uint64_t limit, uint64_t commit_step, uint64_t *old_top, uint64_t *new_top)
{
   for (;;)
   {
      const uint64_t old = (uint64_t)retro_atomic_load_acquire_64(ctr);
      const uint64_t pos = align_up(old & 0xffffffffu, align);
      const uint64_t end = align_up(pos + size, align);
      const uint64_t top = old >> 32;
      uint64_t value;

      if (end > limit)
      {
         /* Failed, and every later attempt with it */
         if ((old & limit) || retro_atomic_cas_64(ctr, (int64_t)old, (int64_t)(old | limit)))
            return JIT_BUMP_FAILED;
         continue;
      }

      value = (old & ~(uint64_t)0xffffffffu) | end;

      if (retro_atomic_cas_64(ctr, (int64_t)old, (int64_t)value))
      {
         *old_top = top;
         *new_top = end > top ? align_up(end, commit_step) : top;
         return pos;
      }
   }
}

void jit_bump_commit(retro_atomic_64_t *ctr, uint64_t top)
{
   for (;;)
   {
      const uint64_t old = (uint64_t)retro_atomic_load_acquire_64(ctr);

      if ((old >> 32) >= top)
         return;

      if (retro_atomic_cas_64(ctr, (int64_t)old, (int64_t)((top << 32) | (old & 0xffffffffu))))
         return;
   }
}

uint64_t jit_bump_used(retro_atomic_64_t *ctr)
{
   return (uint64_t)retro_atomic_load_acquire_64(ctr) & 0xffffffffu;
}

unsigned char *jit_bump_ptr_alloc(retro_atomic_ptr_t *pos, unsigned char *max,
      uintptr_t size, uintptr_t align)
{
   for (;;)
   {
      unsigned char *old = (unsigned char*)retro_atomic_load_acquire_ptr(pos);
      unsigned char *r   = (unsigned char*)(((uintptr_t)old + (align - 1)) & (0 - align));

      if (!(r >= old && r + size > old && r + size <= max))
         return NULL;

      if (retro_atomic_cas_ptr(pos, old, r + size))
         return r;
   }
}
