/* The Vulkan renderer's samplers by their key (vk_sampler_table.h). C89 */

#include "vk_sampler_table.h"

#define MASK (VK_SAMPLER_SLOTS - 1)

/* 64-bit constants from 32-bit halves, as C89 has no long long literal */
#define K64(hi, lo) (((uint64_t)(hi) << 32) | (uint64_t)(lo))

static unsigned home(uint64_t base, uint64_t border)
{
   uint64_t h = base ^ (border * K64(0x9E3779B9u, 0x7F4A7C15u));
   h ^= h >> 29;
   h *= K64(0xBF58476Du, 0x1CE4E5B9u);
   h ^= h >> 32;
   return (unsigned)h & MASK;
}

void *vk_sampler_table_find(const vk_sampler_table_t *t, uint64_t base, uint64_t border)
{
   unsigned i = home(base, border), n;

   for (n = 0; n < VK_SAMPLER_SLOTS && t->obj[i]; n++, i = (i + 1) & MASK)
      if (t->base[i] == base && t->border[i] == border)
         return t->obj[i];
   return 0;
}

int vk_sampler_table_insert(vk_sampler_table_t *t, uint64_t base, uint64_t border, void *obj)
{
   unsigned i = home(base, border), n;

   if (t->count >= VK_SAMPLER_SLOTS - 1)
      return 0;
   for (n = 0; n < VK_SAMPLER_SLOTS; n++, i = (i + 1) & MASK)
   {
      if (!t->obj[i])
      {
         t->base[i]   = base;
         t->border[i] = border;
         t->obj[i]    = obj;
         t->count++;
         return 1;
      }
      if (t->base[i] == base && t->border[i] == border)
         return 0;
   }
   return 0;
}

/* Slot @i emptied: the run after it shifted back over it where its
 * samplers' homes allow, so a probe never stops short of one */
static void take_out(vk_sampler_table_t *t, unsigned i)
{
   unsigned j = i;

   t->obj[i] = 0;
   t->count--;
   for (;;)
   {
      unsigned h;

      j = (j + 1) & MASK;
      if (!t->obj[j])
         return;
      h = home(t->base[j], t->border[j]);
      /* Stays where it is if its home lies in (i, j], cyclically */
      if (i <= j ? (i < h && h <= j) : (i < h || h <= j))
         continue;
      t->base[i]   = t->base[j];
      t->border[i] = t->border[j];
      t->obj[i]    = t->obj[j];
      t->obj[j]    = 0;
      i = j;
   }
}

void vk_sampler_table_collect(vk_sampler_table_t *t, int (*pick)(void *obj, void *ctx), void (*take)(void *obj, void *ctx), void *ctx)
{
   unsigned i = 0;

   while (i < VK_SAMPLER_SLOTS)
   {
      void *obj = t->obj[i];

      if (obj && (!pick || pick(obj, ctx)))
      {
         take_out(t, i);
         take(obj, ctx);
         continue; /* another may have shifted into i */
      }
      i++;
   }
}
