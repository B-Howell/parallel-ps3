/* Vulkan device memory in blocks, handed out as offsets (vk_heap.h). C89. */

#include <stdlib.h>
#include <string.h>

#include "vk_heap.h"

/* What a shared block keeps beside its spans: where each live allocation
 * is, by number, the numbers not in use, and a bit for each allocation
 * freed while the blocks were another thread's. Made with the block */
struct vk_heap_block_side
{
   vk_heap_span_t     spans[VK_HEAP_BLOCK_SPANS];
   vk_heap_span_t     where[VK_HEAP_BLOCK_SPANS];
   unsigned short     numbers[VK_HEAP_BLOCK_SPANS];
   unsigned           number_count;
   retro_atomic_64_t  freed[VK_HEAP_BLOCK_SPANS / 64u];
};

static VkDeviceSize vk_heap_align(VkDeviceSize v, VkDeviceSize a)
{
   if (a <= 1)
      return v;
   return (v + (a - 1)) & ~(a - 1);
}

static unsigned vk_heap_heap_of(const vk_heap_t *heap, unsigned type)
{
   return heap->props.memoryTypes[type].heapIndex;
}

static int vk_heap_host_visible(const vk_heap_t *heap, unsigned type)
{
   return (heap->props.memoryTypes[type].propertyFlags
         & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
}

/* A shared block of this type: the heap's block size, but no more than an
 * eighth of the driver heap it is in, so that one block is never most of
 * a small heap such as a 256 MB BAR window */
static VkDeviceSize vk_heap_block_size(const vk_heap_t *heap, unsigned type)
{
   const unsigned index = vk_heap_heap_of(heap, type);
   VkDeviceSize size    = heap->block_size;

   if (index < heap->props.memoryHeapCount)
   {
      const VkDeviceSize eighth = heap->props.memoryHeaps[index].size / 8u;
      if (eighth && eighth < size)
         size = eighth;
   }
   if (size < 64u * 1024u)
      size = 64u * 1024u;
   return vk_heap_align(size, (VkDeviceSize)(64u * 1024u));
}

void vk_heap_init(vk_heap_t *heap, VkDevice device,
      const VkPhysicalDeviceMemoryProperties *props, const vk_heap_fns_t *fns,
      VkDeviceSize block_size, const VkDeviceSize *heap_limits,
      int dedicated_only)
{
   unsigned i;

   memset(heap, 0, sizeof(*heap));
   heap->device         = device;
   heap->props          = *props;
   heap->fns            = *fns;
   heap->block_size     = block_size ? block_size : (VkDeviceSize)VK_HEAP_BLOCK_SIZE;
   heap->dedicated_only = dedicated_only;
   retro_atomic_int_init(&heap->busy, 0);
   for (i = 0; i < VK_HEAP_MAX_BLOCKS / 64u; i++)
      retro_atomic_64_init(&heap->waiting[i], 0);
   for (i = 0; i < VK_MAX_MEMORY_HEAPS; i++)
      retro_atomic_64_init(&heap->heap_reserved[i], 0);
   if (heap_limits)
      for (i = 0; i < props->memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
         heap->heap_limit[i] = heap_limits[i];
}

/* Counted against the driver heap before it is asked for, so that two
 * threads taking memory at once do not both go over the limit. 0 when it
 * would: nothing is counted then */
static int vk_heap_count(vk_heap_t *heap, unsigned index, VkDeviceSize size)
{
   VkDeviceSize before;

   if (index >= VK_MAX_MEMORY_HEAPS)
      return 0;
   before = (VkDeviceSize)retro_atomic_fetch_add_64(&heap->heap_reserved[index], (int64_t)size);
   if (heap->heap_limit[index] && before + size > heap->heap_limit[index])
   {
      (void)retro_atomic_fetch_sub_64(&heap->heap_reserved[index], (int64_t)size);
      return 0;
   }
   return 1;
}

static void vk_heap_uncount(vk_heap_t *heap, unsigned index, VkDeviceSize size)
{
   if (index < VK_MAX_MEMORY_HEAPS)
      (void)retro_atomic_fetch_sub_64(&heap->heap_reserved[index], (int64_t)size);
}

static void vk_heap_release_block(vk_heap_t *heap, vk_heap_block_t *b)
{
   if (b->mapped)
      heap->fns.unmap_memory(heap->device, b->memory);
   heap->fns.free_memory(heap->device, b->memory, NULL);
   free(b->side);
   vk_heap_uncount(heap, vk_heap_heap_of(heap, b->type), b->size);

   /* A hole, never closed up: every allocation carries its block's slot */
   memset(b, 0, sizeof(*b));
}

void vk_heap_shutdown(vk_heap_t *heap)
{
   unsigned i;

   for (i = 0; i < heap->block_count; i++)
      if (heap->blocks[i].memory != VK_NULL_HANDLE)
         vk_heap_release_block(heap, &heap->blocks[i]);
   heap->block_count = 0;
   heap->bytes_used  = 0;
}

/* One block from the driver, mapped for good when it is host-visible.
 * Returns its slot, or -1 with *result saying why */
static int vk_heap_add_block(vk_heap_t *heap, unsigned type, unsigned linear,
      VkDeviceSize size, unsigned dedicated, VkResult *result)
{
   const unsigned index  = vk_heap_heap_of(heap, type);
   VkMemoryAllocateInfo info;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   vk_heap_block_side_t *side = NULL;
   void *mapped          = NULL;
   vk_heap_block_t *b;
   unsigned slot;
   unsigned i;

   for (slot = 0; slot < heap->block_count; slot++)
      if (heap->blocks[slot].memory == VK_NULL_HANDLE)
         break;

   if (slot >= VK_HEAP_MAX_BLOCKS || !vk_heap_count(heap, index, size))
   {
      *result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      return -1;
   }

   if (!dedicated)
   {
      side = (vk_heap_block_side_t*)malloc(sizeof(*side));
      if (!side)
      {
         vk_heap_uncount(heap, index, size);
         *result = VK_ERROR_OUT_OF_HOST_MEMORY;
         return -1;
      }
      /* Numbers handed out from the lowest */
      for (i = 0; i < VK_HEAP_BLOCK_SPANS; i++)
         side->numbers[i] = (unsigned short)(VK_HEAP_BLOCK_SPANS - 1u - i);
      side->number_count = VK_HEAP_BLOCK_SPANS;
      for (i = 0; i < VK_HEAP_BLOCK_SPANS / 64u; i++)
         retro_atomic_64_init(&side->freed[i], 0);
   }

   memset(&info, 0, sizeof(info));
   info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   info.allocationSize  = size;
   info.memoryTypeIndex = type;

   *result = heap->fns.allocate_memory(heap->device, &info, NULL, &memory);
   if (*result != VK_SUCCESS)
   {
      vk_heap_uncount(heap, index, size);
      free(side);
      return -1;
   }

   if (vk_heap_host_visible(heap, type))
   {
      *result = heap->fns.map_memory(heap->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
      if (*result != VK_SUCCESS)
      {
         heap->fns.free_memory(heap->device, memory, NULL);
         vk_heap_uncount(heap, index, size);
         free(side);
         return -1;
      }
   }

   b = &heap->blocks[slot];
   memset(b, 0, sizeof(*b));
   b->memory    = memory;
   b->mapped    = mapped;
   b->size      = size;
   b->type      = type;
   b->linear    = linear;
   b->dedicated = dedicated;
   b->side      = side;
   b->spans     = side ? side->spans : NULL;
   retro_atomic_int_init(&b->freed, 0);
   if (side)
   {
      side->spans[0].offset = 0;
      side->spans[0].size   = size;
      b->span_count   = 1;
      b->max_free     = size;
   }

   if (slot == heap->block_count)
      heap->block_count++;
   return (int)slot;
}

static void vk_heap_out(vk_heap_t *heap, unsigned slot, VkDeviceSize offset,
      VkDeviceSize size, vk_heap_alloc_t *out)
{
   vk_heap_block_t *b = &heap->blocks[slot];

   b->used          += size;
   b->allocs++;
   heap->bytes_used += size;

   out->memory = b->memory;
   out->offset = offset;
   out->size   = size;
   out->mapped = b->mapped ? (void*)((unsigned char*)b->mapped + offset) : NULL;
   out->block  = slot;
   out->type   = b->type;
   out->number = 0;

   /* There is a number for it: a split is let happen only with room for
    * two more spans than allocations, and there are as many numbers */
   if (b->side)
   {
      const unsigned number = b->side->numbers[--b->side->number_count];
      b->side->where[number].offset = offset;
      b->side->where[number].size   = size;
      out->number = number;
   }
}

/* The lowest span of a shared block that fits. What is left either side
 * stays free */
static int vk_heap_block_alloc(vk_heap_t *heap, unsigned slot,
      VkDeviceSize size, VkDeviceSize align, vk_heap_alloc_t *out)
{
   vk_heap_block_t *b   = &heap->blocks[slot];
   VkDeviceSize largest = 0;
   unsigned i;

   /* A split adds a span: kept within the array, so a free never needs
    * room it does not have (there is never a span more than allocations
    * and one) */
   if (size > b->max_free || b->allocs + 2u > VK_HEAP_BLOCK_SPANS)
      return 0;

   for (i = 0; i < b->span_count; i++)
   {
      vk_heap_span_t *s          = &b->spans[i];
      const VkDeviceSize aligned = vk_heap_align(s->offset, align);
      const VkDeviceSize head    = aligned - s->offset;
      VkDeviceSize tail;

      if (s->size > largest)
         largest = s->size;
      if (head > s->size || size > s->size - head)
         continue;

      tail = s->size - head - size;
      if (head && tail)
      {
         /* The head stays where it is, the tail goes in after it */
         memmove(&b->spans[i + 2], &b->spans[i + 1],
               (b->span_count - i - 1) * sizeof(*s));
         b->spans[i + 1].offset = aligned + size;
         b->spans[i + 1].size   = tail;
         b->span_count++;
         s->size = head;
      }
      else if (head)
         s->size = head;
      else if (tail)
      {
         s->offset = aligned + size;
         s->size   = tail;
      }
      else
      {
         b->span_count--;
         memmove(s, s + 1, (b->span_count - i) * sizeof(*s));
      }

      vk_heap_out(heap, slot, aligned, size, out);
      return 1;
   }

   /* Every span was looked at and none fits: the bound is exact now */
   b->max_free = largest;
   return 0;
}

unsigned vk_heap_trim(vk_heap_t *heap)
{
   unsigned freed = 0;
   unsigned i;

   for (i = 0; i < heap->block_count; i++)
   {
      vk_heap_block_t *b = &heap->blocks[i];
      if (b->memory != VK_NULL_HANDLE && !b->allocs)
      {
         vk_heap_release_block(heap, b);
         freed++;
      }
   }
   return freed;
}

static VkResult vk_heap_alloc_new(vk_heap_t *heap, unsigned type,
      VkDeviceSize size, VkDeviceSize alignment, unsigned linear,
      vk_heap_alloc_t *out)
{
   VkResult result;
   const int slot = vk_heap_add_block(heap, type, linear,
         vk_heap_block_size(heap, type), 0, &result);

   if (slot < 0)
      return result;
   if (vk_heap_block_alloc(heap, (unsigned)slot, size, alignment, out))
      return VK_SUCCESS;
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

VkResult vk_heap_alloc(vk_heap_t *heap, unsigned type, VkDeviceSize size,
      VkDeviceSize alignment, int linear, vk_heap_alloc_t *out)
{
   const unsigned kind = linear ? 1u : 0u;
   VkResult result;
   unsigned i;

   memset(out, 0, sizeof(*out));
   if (type >= heap->props.memoryTypeCount || !size)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   if (!alignment)
      alignment = 1;

   /* Big, or asked to be: a block of its own */
   if (heap->dedicated_only || size > vk_heap_block_size(heap, type) / 2u)
   {
      int slot = vk_heap_add_block(heap, type, kind, size, 1, &result);
      if (slot < 0 && result == VK_ERROR_OUT_OF_DEVICE_MEMORY && vk_heap_trim(heap))
         slot = vk_heap_add_block(heap, type, kind, size, 1, &result);
      if (slot < 0)
         return result;
      vk_heap_out(heap, (unsigned)slot, 0, size, out);
      return VK_SUCCESS;
   }

   for (i = 0; i < heap->block_count; i++)
   {
      const vk_heap_block_t *b = &heap->blocks[i];
      if (b->memory == VK_NULL_HANDLE || b->dedicated
            || b->type != type || b->linear != kind)
         continue;
      if (vk_heap_block_alloc(heap, i, size, alignment, out))
         return VK_SUCCESS;
   }

   result = vk_heap_alloc_new(heap, type, size, alignment, kind, out);

   /* The driver has none left, or the limit is reached: the empty blocks
    * of the other types and kinds go back, and it is asked once more */
   if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && vk_heap_trim(heap))
      result = vk_heap_alloc_new(heap, type, size, alignment, kind, out);
   return result;
}

/* An empty shared block is kept for what comes next, unless another empty
 * one of its type and kind is already */
static void vk_heap_emptied(vk_heap_t *heap, unsigned slot)
{
   vk_heap_block_t *b = &heap->blocks[slot];
   unsigned i;

   for (i = 0; i < heap->block_count; i++)
   {
      const vk_heap_block_t *o = &heap->blocks[i];
      if (i != slot && o->memory != VK_NULL_HANDLE && !o->dedicated
            && !o->allocs && o->type == b->type && o->linear == b->linear)
      {
         vk_heap_release_block(heap, b);
         return;
      }
   }
}

int vk_heap_free(vk_heap_t *heap, const vk_heap_alloc_t *alloc)
{
   vk_heap_block_t *b;
   VkDeviceSize offset;
   VkDeviceSize size;
   unsigned lo;
   unsigned hi;
   int joins_prev;
   int joins_next;

   if (!alloc || alloc->memory == VK_NULL_HANDLE || alloc->block >= heap->block_count)
      return 0;
   if (alloc->number >= VK_HEAP_BLOCK_SPANS)
      return 0;

   /* The block it came from and no other: the slot may hold another
    * block now */
   b = &heap->blocks[alloc->block];
   if (b->memory != alloc->memory || !b->allocs || !alloc->size
         || alloc->offset > b->size || alloc->size > b->size - alloc->offset)
      return 0;

   if (b->dedicated)
   {
      if (alloc->offset || alloc->size != b->size)
         return 0;
      heap->bytes_used -= b->size;
      vk_heap_release_block(heap, b);
      return 1;
   }

   offset = alloc->offset;
   size   = alloc->size;

   /* Where it goes: the first span that starts after it */
   lo = 0;
   hi = b->span_count;
   while (lo < hi)
   {
      const unsigned mid = lo + (hi - lo) / 2u;
      if (b->spans[mid].offset < offset)
         lo = mid + 1;
      else
         hi = mid;
   }

   /* Memory that is free already is not freed again: its span would be
    * on the list twice and handed out to two */
   if ((lo > 0 && b->spans[lo - 1].offset + b->spans[lo - 1].size > offset)
         || (lo < b->span_count && offset + size > b->spans[lo].offset))
      return 0;

   joins_prev = lo > 0 && b->spans[lo - 1].offset + b->spans[lo - 1].size == offset;
   joins_next = lo < b->span_count && offset + size == b->spans[lo].offset;

   if (joins_prev && joins_next)
   {
      b->spans[lo - 1].size += size + b->spans[lo].size;
      size = b->spans[lo - 1].size;
      b->span_count--;
      memmove(&b->spans[lo], &b->spans[lo + 1], (b->span_count - lo) * sizeof(*b->spans));
   }
   else if (joins_prev)
   {
      b->spans[lo - 1].size += size;
      size = b->spans[lo - 1].size;
   }
   else if (joins_next)
   {
      b->spans[lo].offset  = offset;
      b->spans[lo].size   += size;
      size = b->spans[lo].size;
   }
   else
   {
      memmove(&b->spans[lo + 1], &b->spans[lo], (b->span_count - lo) * sizeof(*b->spans));
      b->spans[lo].offset = offset;
      b->spans[lo].size   = size;
      b->span_count++;
   }

   if (size > b->max_free)
      b->max_free = size;

   b->side->numbers[b->side->number_count++] = (unsigned short)alloc->number;
   b->used          -= alloc->size;
   heap->bytes_used -= alloc->size;
   if (!--b->allocs)
      vk_heap_emptied(heap, alloc->block);
   return 1;
}

VkDeviceSize vk_heap_reserved(vk_heap_t *heap, unsigned index)
{
   if (index >= VK_MAX_MEMORY_HEAPS)
      return 0;
   return (VkDeviceSize)retro_atomic_load_acquire_64(&heap->heap_reserved[index]);
}

/* The frees left while the blocks were another thread's, by the thread
 * that has them now. A free leaves its allocation's bit, then its block's:
 * a block's bit taken here is followed by every bit that was left in it
 * before */
static void vk_heap_take_frees(vk_heap_t *heap)
{
   unsigned w;

   for (w = 0; w < VK_HEAP_MAX_BLOCKS / 64u; w++)
   {
      uint64_t blocks;

      if (!retro_atomic_load_acquire_64(&heap->waiting[w]))
         continue;
      blocks = (uint64_t)retro_atomic_exchange_64(&heap->waiting[w], 0);

      while (blocks)
      {
         unsigned bit = 0;
         unsigned slot;
         vk_heap_block_t *b;
         vk_heap_alloc_t a;

         while (!((blocks >> bit) & 1u))
            bit++;
         blocks &= ~((uint64_t)1 << bit);
         slot = w * 64u + bit;
         b    = &heap->blocks[slot];

         memset(&a, 0, sizeof(a));
         a.memory = b->memory;
         a.block  = slot;
         a.type   = b->type;

         if (!b->side)
         {
            if (retro_atomic_exchange_int(&b->freed, 0))
            {
               a.size = b->size;
               vk_heap_free(heap, &a);
            }
            continue;
         }

         {
            unsigned i;
            for (i = 0; i < VK_HEAP_BLOCK_SPANS / 64u; i++)
            {
               uint64_t numbers;
               vk_heap_block_side_t *side = b->side;

               if (!side || !retro_atomic_load_acquire_64(&side->freed[i]))
                  continue;
               numbers = (uint64_t)retro_atomic_exchange_64(&side->freed[i], 0);
               while (numbers)
               {
                  unsigned n = 0;
                  while (!((numbers >> n) & 1u))
                     n++;
                  numbers &= ~((uint64_t)1 << n);
                  a.number = i * 64u + n;
                  a.offset = side->where[a.number].offset;
                  a.size   = side->where[a.number].size;
                  /* The last free of a block may give it back: its side
                   * goes with it, after this word, which held the last */
                  vk_heap_free(heap, &a);
                  side = b->side;
               }
            }
         }
      }
   }
}

/* A VkDeviceMemory of its own from the driver, within the limit */
static VkResult vk_heap_alloc_loose(vk_heap_t *heap, unsigned type,
      VkDeviceSize size, vk_heap_alloc_t *out)
{
   const unsigned index = vk_heap_heap_of(heap, type);
   VkMemoryAllocateInfo info;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   void *mapped          = NULL;
   VkResult result;

   if (!vk_heap_count(heap, index, size))
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   memset(&info, 0, sizeof(info));
   info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   info.allocationSize  = size;
   info.memoryTypeIndex = type;

   result = heap->fns.allocate_memory(heap->device, &info, NULL, &memory);
   if (result == VK_SUCCESS && vk_heap_host_visible(heap, type))
   {
      result = heap->fns.map_memory(heap->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
      if (result != VK_SUCCESS)
         heap->fns.free_memory(heap->device, memory, NULL);
   }
   if (result != VK_SUCCESS)
   {
      vk_heap_uncount(heap, index, size);
      return result;
   }

   memset(out, 0, sizeof(*out));
   out->memory = memory;
   out->size   = size;
   out->mapped = mapped;
   out->block  = VK_HEAP_LOOSE;
   out->type   = type;
   return VK_SUCCESS;
}

VkResult vk_heap_alloc_shared(vk_heap_t *heap, const unsigned *types,
      unsigned type_count, VkDeviceSize size, VkDeviceSize alignment,
      int linear, vk_heap_alloc_t *out, unsigned *type)
{
   VkResult result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
   unsigned i;

   memset(out, 0, sizeof(*out));
   if (!size)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   if (retro_atomic_cas_int(&heap->busy, 0, 1))
   {
      vk_heap_take_frees(heap);
      for (i = 0; i < type_count; i++)
      {
         result = vk_heap_alloc(heap, types[i], size, alignment, linear, out);
         if (result == VK_SUCCESS)
         {
            *type = types[i];
            break;
         }
      }
      retro_atomic_store_release_int(&heap->busy, 0);
      return result;
   }

   for (i = 0; i < type_count; i++)
   {
      if (types[i] >= heap->props.memoryTypeCount)
         continue;
      result = vk_heap_alloc_loose(heap, types[i], size, out);
      if (result == VK_SUCCESS)
      {
         *type = types[i];
         break;
      }
   }
   return result;
}

void vk_heap_free_shared(vk_heap_t *heap, const vk_heap_alloc_t *alloc)
{
   vk_heap_block_t *b;

   if (!alloc || alloc->memory == VK_NULL_HANDLE)
      return;

   if (alloc->block == VK_HEAP_LOOSE)
   {
      const unsigned index = vk_heap_heap_of(heap, alloc->type);
      if (alloc->mapped)
         heap->fns.unmap_memory(heap->device, alloc->memory);
      heap->fns.free_memory(heap->device, alloc->memory, NULL);
      vk_heap_uncount(heap, index, alloc->size);
      return;
   }

   if (retro_atomic_cas_int(&heap->busy, 0, 1))
   {
      vk_heap_take_frees(heap);
      vk_heap_free(heap, alloc);
      retro_atomic_store_release_int(&heap->busy, 0);
      return;
   }

   /* The blocks are another thread's: a bit for whoever has them next. The
    * block stays while the allocation is live, so its side is there */
   if (alloc->block >= VK_HEAP_MAX_BLOCKS || alloc->number >= VK_HEAP_BLOCK_SPANS)
      return;
   b = &heap->blocks[alloc->block];
   if (b->side)
      (void)retro_atomic_fetch_or_64(&b->side->freed[alloc->number / 64u],
            (int64_t)((uint64_t)1 << (alloc->number % 64u)));
   else
      retro_atomic_store_release_int(&b->freed, 1);
   (void)retro_atomic_fetch_or_64(&heap->waiting[alloc->block / 64u],
         (int64_t)((uint64_t)1 << (alloc->block % 64u)));
}
