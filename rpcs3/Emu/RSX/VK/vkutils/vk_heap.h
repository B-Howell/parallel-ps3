/* Vulkan device memory for the renderer: taken from the driver in large
 * blocks and handed out as offsets inside them. C89.
 *
 * In place of the Vulkan Memory Allocator. The renderer asks for memory
 * of one memory type at a time (it walks its own list of types), with a
 * size and an alignment, for a buffer or linear image ("linear") or for
 * an optimal image, and gets back the VkDeviceMemory, the offset in it
 * and, for host-visible memory, its address.
 *
 * - A block is one VkDeviceMemory of VK_HEAP_BLOCK_SIZE (less on a small
 *   driver heap, such as a 256 MB BAR window). Anything over half a block
 *   is a block of its own ("dedicated"), given back to the driver when it
 *   is freed. With dedicated_only every allocation is.
 * - Buffers and linear images never share a block with optimal images, so
 *   bufferImageGranularity is kept without padding anything.
 * - A host-visible block is mapped once, when it is taken, and stays
 *   mapped: an address is the block's plus the offset; nothing maps or
 *   unmaps per use.
 * - The free spans of a block are kept sorted by offset and never
 *   adjacent: an allocation takes the lowest span that fits, a free finds
 *   its place by binary search and merges with the span either side.
 *   The span array is made with the block, at a fixed capacity, and an
 *   allocation that could overflow it goes to another block: nothing is
 *   allocated on the host but when a block is taken from the driver.
 * - An empty block is kept for the next allocation, unless another empty
 *   block of its type and kind is kept already; when the driver has
 *   nothing left, empty blocks are given back and it is asked once more.
 * - A heap may be given a ceiling per driver heap (the VRAM limit).
 *
 * Any thread may allocate and free with vk_heap_alloc_shared and
 * vk_heap_free_shared, and none waits for another. The blocks are one
 * thread's at a time, taken with one compare-and-swap: a thread that finds
 * them taken allocates a VkDeviceMemory of its own from the driver instead
 * ("loose", freed straight back to it), and a thread that frees while they
 * are taken leaves a bit for the allocation, which whoever takes the blocks
 * next frees. A free that is waiting holds nothing up but the reuse of
 * what it frees. vk_heap_alloc, vk_heap_free and vk_heap_trim are the
 * blocks' own and are for the thread that has them (or for a heap only one
 * thread uses). */

#ifndef VK_HEAP_H
#define VK_HEAP_H

#include <stddef.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VK_HEAP_BLOCK_SIZE   (64u * 1024u * 1024u)
#define VK_HEAP_MAX_BLOCKS   1024u
/* Free spans of a shared block, and so at most this less one allocations
 * in it */
#define VK_HEAP_BLOCK_SPANS  4096u
/* An allocation's block when it is a VkDeviceMemory of its own, taken while
 * the blocks were another thread's */
#define VK_HEAP_LOOSE        0xFFFFFFFFu

/* The entry points, passed in: the loader that has them is the caller's */
typedef struct vk_heap_fns
{
   PFN_vkAllocateMemory allocate_memory;
   PFN_vkFreeMemory     free_memory;
   PFN_vkMapMemory      map_memory;
   PFN_vkUnmapMemory    unmap_memory;
} vk_heap_fns_t;

typedef struct vk_heap_span
{
   VkDeviceSize offset;
   VkDeviceSize size;
} vk_heap_span_t;

/* Where each live allocation of a shared block is, by its number, with
 * the numbers free and the bits of those freed while the blocks were
 * another thread's (vk_heap.c) */
typedef struct vk_heap_block_side vk_heap_block_side_t;

typedef struct vk_heap_block
{
   VkDeviceMemory  memory;      /* VK_NULL_HANDLE: the slot is free */
   void           *mapped;      /* the whole block, or NULL */
   VkDeviceSize    size;
   VkDeviceSize    used;
   /* No free span is larger. Exact after a search that found nothing, an
    * upper bound otherwise */
   VkDeviceSize    max_free;
   vk_heap_span_t *spans;       /* NULL for a dedicated block */
   vk_heap_block_side_t *side;  /* NULL for a dedicated block */
   /* A dedicated block's one allocation, freed while the blocks were
    * another thread's */
   retro_atomic_int_t freed;
   unsigned        span_count;
   unsigned        allocs;
   unsigned        type;
   unsigned        linear;
   unsigned        dedicated;
} vk_heap_block_t;

/* Where an allocation is. block is the slot it came from; mapped already
 * has the offset in it, or is NULL for memory that is not host-visible */
typedef struct vk_heap_alloc
{
   VkDeviceMemory memory;
   VkDeviceSize   offset;
   VkDeviceSize   size;
   void          *mapped;
   unsigned       block;
   unsigned       type;
   unsigned       number;      /* in its shared block */
} vk_heap_alloc_t;

typedef struct vk_heap
{
   VkDevice                         device;
   vk_heap_fns_t                    fns;
   VkPhysicalDeviceMemoryProperties props;
   VkDeviceSize                     block_size;
   int                              dedicated_only;
   /* Taken from the driver, per driver heap (vk_heap_reserved), loose
    * allocations with it, and the most that may be */
   retro_atomic_64_t                heap_reserved[VK_MAX_MEMORY_HEAPS];
   VkDeviceSize                     heap_limit[VK_MAX_MEMORY_HEAPS];
   /* 1 while a thread has the blocks */
   retro_atomic_int_t               busy;
   /* A bit for each block with frees waiting in it */
   retro_atomic_64_t                waiting[VK_HEAP_MAX_BLOCKS / 64u];
   VkDeviceSize                     bytes_used;
   unsigned                         block_count; /* slots ever used */
   vk_heap_block_t                  blocks[VK_HEAP_MAX_BLOCKS];
} vk_heap_t;

/* heap_limits: the most that may be taken from each driver heap, or NULL
 * for no limit (a 0 in it is no limit too). block_size 0 is
 * VK_HEAP_BLOCK_SIZE. */
void vk_heap_init(vk_heap_t *heap, VkDevice device,
      const VkPhysicalDeviceMemoryProperties *props, const vk_heap_fns_t *fns,
      VkDeviceSize block_size, const VkDeviceSize *heap_limits,
      int dedicated_only);

/* Gives every block back to the driver */
void vk_heap_shutdown(vk_heap_t *heap);

/* Room for size bytes at alignment (a power of two) in memory type type.
 * VK_ERROR_OUT_OF_DEVICE_MEMORY when no block has room and none can be
 * had within the limit, or what the driver said. */
VkResult vk_heap_alloc(vk_heap_t *heap, unsigned type, VkDeviceSize size,
      VkDeviceSize alignment, int linear, vk_heap_alloc_t *out);

/* A free of memory already free, or of a block since given back, changes
 * nothing and returns 0 */
int vk_heap_free(vk_heap_t *heap, const vk_heap_alloc_t *alloc);

/* Gives empty blocks back to the driver. Returns how many */
unsigned vk_heap_trim(vk_heap_t *heap);

/* From any thread. Room for size bytes at alignment in the first of the
 * types (in order) that has it; *type is the one it came from. When the
 * blocks are another thread's, a VkDeviceMemory of its own. */
VkResult vk_heap_alloc_shared(vk_heap_t *heap, const unsigned *types,
      unsigned type_count, VkDeviceSize size, VkDeviceSize alignment,
      int linear, vk_heap_alloc_t *out, unsigned *type);

/* From any thread: freed now, or when the blocks are next taken */
void vk_heap_free_shared(vk_heap_t *heap, const vk_heap_alloc_t *alloc);

/* What is taken from driver heap index, in bytes: one load */
VkDeviceSize vk_heap_reserved(vk_heap_t *heap, unsigned index);

#ifdef __cplusplus
}
#endif

#endif
