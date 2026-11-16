/* The Vulkan renderer's device memory heap (vk_heap.c), on a driver of the
 * test's own: what it hands out never overlaps, keeps its alignment, its
 * address and its accounting, and goes back whole. Buffers and optimal
 * images never share a VkDeviceMemory, big allocations get one of their
 * own, a double free is refused, the limit holds and empty blocks go back
 * when it is reached, a block's spans never outgrow their array, and once
 * warm a steady run asks the driver for nothing. Shared: a thread that
 * finds the blocks another's takes memory of its own and frees it straight
 * back, a free left meanwhile is freed when the blocks are next taken, and
 * threads allocating and freeing at once (VK_HEAP_TEST_THREADS, the
 * ThreadSanitizer build) never hand out the same memory twice and give it
 * all back. C89. */

#include <stdlib.h>
#include <string.h>

#include "../../Emu/RSX/VK/vkutils/vk_heap.h"
#include "test_common.h"

#ifdef VK_HEAP_TEST_THREADS
#include <rthreads/rthreads.h>
#endif

#define MB ((VkDeviceSize)1024u * 1024u)

/* Counted with atomics: the threaded run calls the driver from several
 * threads, as the renderer may */
static retro_atomic_int_t driver_allocs_a;
static retro_atomic_int_t driver_frees_a;
static retro_atomic_int_t driver_live_a;
static retro_atomic_int_t driver_maps_a;
static retro_atomic_int_t driver_unmaps_a;
static retro_atomic_int_t next_handle_a;
#define driver_allocs ((unsigned)retro_atomic_load_acquire_int(&driver_allocs_a))
#define driver_frees  ((unsigned)retro_atomic_load_acquire_int(&driver_frees_a))
#define driver_live   ((unsigned)retro_atomic_load_acquire_int(&driver_live_a))
#define driver_maps   ((unsigned)retro_atomic_load_acquire_int(&driver_maps_a))
#define driver_unmaps ((unsigned)retro_atomic_load_acquire_int(&driver_unmaps_a))

static VKAPI_ATTR VkResult VKAPI_CALL fake_allocate(VkDevice d,
      const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *a,
      VkDeviceMemory *out)
{
   (void)d; (void)a; (void)info;
   retro_atomic_inc_int(&driver_allocs_a);
   retro_atomic_inc_int(&driver_live_a);
   *out = (VkDeviceMemory)(size_t)(retro_atomic_fetch_add_int(&next_handle_a, 1) + 1);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL fake_free(VkDevice d, VkDeviceMemory m,
      const VkAllocationCallbacks *a)
{
   (void)d; (void)a; (void)m;
   retro_atomic_inc_int(&driver_frees_a);
   retro_atomic_dec_int(&driver_live_a);
}

/* An address no two blocks share, never written through */
static VKAPI_ATTR VkResult VKAPI_CALL fake_map(VkDevice d, VkDeviceMemory m,
      VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void **out)
{
   (void)d; (void)offset; (void)size; (void)flags;
   retro_atomic_inc_int(&driver_maps_a);
   *out = (void*)((size_t)m << 24);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL fake_unmap(VkDevice d, VkDeviceMemory m)
{
   (void)d; (void)m;
   retro_atomic_inc_int(&driver_unmaps_a);
}

/* Device-local 4 GB, system 2 GB, a 256 MB BAR window */
static void props_of(VkPhysicalDeviceMemoryProperties *p)
{
   memset(p, 0, sizeof(*p));
   p->memoryHeapCount        = 3;
   p->memoryHeaps[0].size    = 4096 * MB;
   p->memoryHeaps[0].flags   = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
   p->memoryHeaps[1].size    = 2048 * MB;
   p->memoryHeaps[2].size    = 256 * MB;
   p->memoryHeaps[2].flags   = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
   p->memoryTypeCount        = 3;
   p->memoryTypes[0].heapIndex     = 0;
   p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   p->memoryTypes[1].heapIndex     = 1;
   p->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   p->memoryTypes[2].heapIndex     = 2;
   p->memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
      | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}

static const vk_heap_fns_t fns = { fake_allocate, fake_free, fake_map, fake_unmap };

static unsigned long rng = 12345;
static unsigned rnd(unsigned n)
{
   rng = rng * 1103515245ul + 12345ul;
   return (unsigned)((rng >> 8) % n);
}

typedef struct live
{
   vk_heap_alloc_t a;
   int linear;
   int in_use;
} live_t;

#define LIVE 3000
static live_t lives[LIVE];

static int overlaps(const vk_heap_alloc_t *x, const vk_heap_alloc_t *y)
{
   return x->memory == y->memory
      && x->offset < y->offset + y->size && y->offset < x->offset + x->size;
}

static void check_all(const vk_heap_t *heap)
{
   VkDeviceSize used = 0;
   unsigned i, j;
   int bad_overlap = 0, bad_kind = 0;

   for (i = 0; i < LIVE; i++)
   {
      if (!lives[i].in_use)
         continue;
      used += lives[i].a.size;
      for (j = i + 1; j < LIVE; j++)
      {
         if (!lives[j].in_use)
            continue;
         if (overlaps(&lives[i].a, &lives[j].a))
            bad_overlap++;
         if (lives[i].a.memory == lives[j].a.memory && lives[i].linear != lives[j].linear)
            bad_kind++;
      }
   }
   TEST_CHECK(!bad_overlap);
   TEST_CHECK(!bad_kind);
   TEST_CHECK(used == heap->bytes_used);
}

/* Each block's spans: sorted, apart, inside it; free and used add up */
static void check_blocks(const vk_heap_t *heap)
{
   unsigned i, j;
   int bad = 0;

   for (i = 0; i < heap->block_count; i++)
   {
      const vk_heap_block_t *b = &heap->blocks[i];
      VkDeviceSize free_bytes = 0;

      if (b->memory == VK_NULL_HANDLE || b->dedicated)
         continue;
      if (b->span_count > VK_HEAP_BLOCK_SPANS || b->span_count > b->allocs + 1)
         bad++;
      for (j = 0; j < b->span_count; j++)
      {
         free_bytes += b->spans[j].size;
         if (!b->spans[j].size || b->spans[j].offset + b->spans[j].size > b->size)
            bad++;
         if (j && b->spans[j - 1].offset + b->spans[j - 1].size >= b->spans[j].offset)
            bad++;
      }
      if (free_bytes + b->used != b->size)
         bad++;
   }
   TEST_CHECK(!bad);
}

static void random_run(vk_heap_t *heap, unsigned rounds)
{
   static const VkDeviceSize aligns[] = { 1, 4, 16, 256, 1024, 4096, 65536 };
   unsigned r;

   for (r = 0; r < rounds; r++)
   {
      live_t *l = &lives[rnd(LIVE)];

      if (l->in_use)
      {
         TEST_CHECK(vk_heap_free(heap, &l->a));
         l->in_use = 0;
      }
      else
      {
         const VkDeviceSize align = aligns[rnd(7)];
         /* Mostly small, some up to a few MB, now and then one big
          * enough for a block of its own */
         VkDeviceSize size = 1 + rnd(64 * 1024);
         const unsigned type  = rnd(3);
         const int linear     = (int)rnd(2);

         if (!rnd(8))
            size = 1 + rnd(4 * 1024 * 1024);
         if (!rnd(200))
            size = 20 * MB + rnd(1024);

         if (vk_heap_alloc(heap, type, size, align, linear, &l->a) != VK_SUCCESS)
         {
            TEST_CHECK(0);
            continue;
         }
         TEST_CHECK(l->a.offset % align == 0);
         TEST_CHECK(l->a.size == size);
         TEST_CHECK(l->a.type == type);
         /* Host-visible memory has its address, and only it */
         if (type == 0)
            TEST_CHECK(l->a.mapped == NULL);
         else
            TEST_CHECK(l->a.mapped == (void*)(((size_t)l->a.memory << 24) + (size_t)l->a.offset));
         l->linear = linear;
         l->in_use = 1;
      }

      if (r % 9973 == 0)
      {
         check_all(heap);
         check_blocks(heap);
      }
   }
}

static void free_all(vk_heap_t *heap)
{
   unsigned i;
   for (i = 0; i < LIVE; i++)
      if (lives[i].in_use)
      {
         TEST_CHECK(vk_heap_free(heap, &lives[i].a));
         lives[i].in_use = 0;
      }
}

/* At most one empty shared block of each type and kind is kept */
static void check_empty_kept(const vk_heap_t *heap)
{
   unsigned counts[VK_MAX_MEMORY_TYPES][2];
   unsigned i;
   int bad = 0;

   memset(counts, 0, sizeof(counts));
   for (i = 0; i < heap->block_count; i++)
   {
      const vk_heap_block_t *b = &heap->blocks[i];
      if (b->memory == VK_NULL_HANDLE)
         continue;
      if (b->allocs || b->dedicated)
         bad++;
      else if (++counts[b->type][b->linear] > 1)
         bad++;
   }
   TEST_CHECK(!bad);
}

static void random_and_whole(void)
{
   VkPhysicalDeviceMemoryProperties props;
   vk_heap_t *heap = (vk_heap_t*)malloc(sizeof(*heap));
   unsigned before;

   props_of(&props);
   vk_heap_init(heap, (VkDevice)0, &props, &fns, 0, NULL, 0);

   random_run(heap, 200000);
   check_all(heap);
   check_blocks(heap);
   free_all(heap);
   TEST_CHECK(heap->bytes_used == 0);
   check_empty_kept(heap);

   /* Warm now: the same mix of small allocations asks the driver for no
    * shared block it has not given already */
   random_run(heap, 4000);
   free_all(heap);
   before = driver_allocs;
   rng = 777;
   {
      unsigned i;
      for (i = 0; i < 64; i++)
      {
         live_t *l = &lives[i];
         TEST_CHECK(vk_heap_alloc(heap, i % 3, 4096, 256, (int)(i & 1), &l->a) == VK_SUCCESS);
         l->linear = (int)(i & 1);
         l->in_use = 1;
      }
   }
   TEST_CHECK(driver_allocs == before);
   free_all(heap);

   vk_heap_shutdown(heap);
   TEST_CHECK(driver_live == 0);
   TEST_CHECK(driver_maps == driver_unmaps);
   free(heap);
}

static void double_free_and_dedicated(void)
{
   VkPhysicalDeviceMemoryProperties props;
   vk_heap_t *heap = (vk_heap_t*)malloc(sizeof(*heap));
   vk_heap_alloc_t a, b, c, big;
   unsigned live_before;

   props_of(&props);
   vk_heap_init(heap, (VkDevice)0, &props, &fns, 0, NULL, 0);

   TEST_CHECK(vk_heap_alloc(heap, 0, 1000, 256, 1, &a) == VK_SUCCESS);
   TEST_CHECK(vk_heap_alloc(heap, 0, 1000, 256, 1, &b) == VK_SUCCESS);
   TEST_CHECK(vk_heap_free(heap, &a));
   TEST_CHECK(!vk_heap_free(heap, &a));
   /* What a took is handed out once more, not twice */
   TEST_CHECK(vk_heap_alloc(heap, 0, 1000, 256, 1, &c) == VK_SUCCESS);
   TEST_CHECK(c.memory == a.memory && c.offset == a.offset);

   /* Over half a block: one of its own, back to the driver at its free */
   live_before = driver_live;
   TEST_CHECK(vk_heap_alloc(heap, 0, 40 * MB, 256, 0, &big) == VK_SUCCESS);
   TEST_CHECK(big.offset == 0 && big.memory != a.memory);
   TEST_CHECK(driver_live == live_before + 1);
   TEST_CHECK(vk_heap_free(heap, &big));
   TEST_CHECK(driver_live == live_before);
   TEST_CHECK(!vk_heap_free(heap, &big));

   /* The BAR window's blocks are an eighth of it */
   TEST_CHECK(vk_heap_alloc(heap, 2, 1000, 1, 1, &a) == VK_SUCCESS);
   TEST_CHECK(heap->blocks[a.block].size == 32 * MB);
   TEST_CHECK(vk_heap_free(heap, &a));

   TEST_CHECK(vk_heap_free(heap, &b));
   TEST_CHECK(vk_heap_free(heap, &c));
   TEST_CHECK(heap->bytes_used == 0);
   vk_heap_shutdown(heap);
   TEST_CHECK(driver_live == 0);
   free(heap);
}

/* Every allocation a VkDeviceMemory of its own, as with the allocator
 * disabled */
static void dedicated_only(void)
{
   VkPhysicalDeviceMemoryProperties props;
   vk_heap_t *heap = (vk_heap_t*)malloc(sizeof(*heap));
   vk_heap_alloc_t a, b;

   props_of(&props);
   vk_heap_init(heap, (VkDevice)0, &props, &fns, 0, NULL, 1);
   TEST_CHECK(vk_heap_alloc(heap, 1, 100, 16, 1, &a) == VK_SUCCESS);
   TEST_CHECK(vk_heap_alloc(heap, 1, 100, 16, 1, &b) == VK_SUCCESS);
   TEST_CHECK(a.memory != b.memory && !a.offset && !b.offset);
   TEST_CHECK(vk_heap_reserved(heap, 1) == 200);
   TEST_CHECK(vk_heap_free(heap, &a));
   TEST_CHECK(vk_heap_free(heap, &b));
   TEST_CHECK(driver_live == 0 && vk_heap_reserved(heap, 1) == 0);
   vk_heap_shutdown(heap);
   free(heap);
}

/* A device-local limit of 256 MB: four 64 MB blocks and no more; empty
 * blocks of the other kind go back for what is asked then */
static void limit(void)
{
   VkPhysicalDeviceMemoryProperties props;
   VkDeviceSize limits[VK_MAX_MEMORY_HEAPS];
   vk_heap_t *heap = (vk_heap_t*)malloc(sizeof(*heap));
   vk_heap_alloc_t a[8], none;
   unsigned i;

   memset(limits, 0, sizeof(limits));
   limits[0] = 256 * MB;
   props_of(&props);
   vk_heap_init(heap, (VkDevice)0, &props, &fns, 0, limits, 0);

   for (i = 0; i < 4; i++)
      TEST_CHECK(vk_heap_alloc(heap, 0, 30 * MB, 256, 0, &a[i]) == VK_SUCCESS);
   /* 2 x 30 MB fill a block: two blocks of optimal images */
   TEST_CHECK(vk_heap_reserved(heap, 0) == 128 * MB);
   for (i = 4; i < 8; i++)
      TEST_CHECK(vk_heap_alloc(heap, 0, 30 * MB, 256, 1, &a[i]) == VK_SUCCESS);
   TEST_CHECK(vk_heap_reserved(heap, 0) == 256 * MB);
   TEST_CHECK(vk_heap_alloc(heap, 0, 30 * MB, 256, 1, &none) == VK_ERROR_OUT_OF_DEVICE_MEMORY);

   /* The images go; the second of their empty blocks goes back at once,
    * the first is kept - until the buffers need it */
   for (i = 0; i < 4; i++)
      TEST_CHECK(vk_heap_free(heap, &a[i]));
   TEST_CHECK(vk_heap_reserved(heap, 0) == 192 * MB);
   TEST_CHECK(vk_heap_alloc(heap, 0, 30 * MB, 256, 1, &a[0]) == VK_SUCCESS);
   TEST_CHECK(vk_heap_alloc(heap, 0, 30 * MB, 256, 1, &a[1]) == VK_SUCCESS);
   TEST_CHECK(vk_heap_reserved(heap, 0) == 256 * MB);
   TEST_CHECK(vk_heap_alloc(heap, 0, 30 * MB, 256, 1, &a[2]) == VK_SUCCESS);
   TEST_CHECK(vk_heap_alloc(heap, 0, 30 * MB, 256, 1, &a[3]) == VK_SUCCESS);
   TEST_CHECK(vk_heap_reserved(heap, 0) == 256 * MB);
   TEST_CHECK(vk_heap_alloc(heap, 0, 30 * MB, 256, 1, &none) == VK_ERROR_OUT_OF_DEVICE_MEMORY);

   for (i = 0; i < 4; i++)
      TEST_CHECK(vk_heap_free(heap, &a[i]));
   for (i = 4; i < 8; i++)
      TEST_CHECK(vk_heap_free(heap, &a[i]));
   TEST_CHECK(heap->bytes_used == 0);
   vk_heap_shutdown(heap);
   TEST_CHECK(driver_live == 0);
   free(heap);
}

/* More allocations than a block has spans for: the rest go to another
 * block, and freeing every other one (each a span of its own) fits */
static void spans(void)
{
   VkPhysicalDeviceMemoryProperties props;
   vk_heap_t *heap = (vk_heap_t*)malloc(sizeof(*heap));
   vk_heap_alloc_t *a = (vk_heap_alloc_t*)malloc(2 * VK_HEAP_BLOCK_SPANS * sizeof(*a));
   unsigned i;

   props_of(&props);
   vk_heap_init(heap, (VkDevice)0, &props, &fns, 0, NULL, 0);
   for (i = 0; i < 2 * VK_HEAP_BLOCK_SPANS; i++)
      TEST_CHECK(vk_heap_alloc(heap, 0, 256, 256, 1, &a[i]) == VK_SUCCESS);
   TEST_CHECK(a[0].memory != a[2 * VK_HEAP_BLOCK_SPANS - 1].memory);
   for (i = 0; i < 2 * VK_HEAP_BLOCK_SPANS; i += 2)
      TEST_CHECK(vk_heap_free(heap, &a[i]));
   check_blocks(heap);
   for (i = 1; i < 2 * VK_HEAP_BLOCK_SPANS; i += 2)
      TEST_CHECK(vk_heap_free(heap, &a[i]));
   TEST_CHECK(heap->bytes_used == 0);
   check_blocks(heap);
   vk_heap_shutdown(heap);
   free(a);
   free(heap);
}

/* The blocks another thread's: memory of its own, straight back; a free
 * of a shared allocation waits for the blocks to be taken again */
static void shared_one_thread(void)
{
   static const unsigned types[2] = { 0, 1 };
   VkPhysicalDeviceMemoryProperties props;
   vk_heap_t *heap = (vk_heap_t*)malloc(sizeof(*heap));
   vk_heap_alloc_t a, b, c, loose;
   unsigned type = 99;
   unsigned live_before;

   props_of(&props);
   vk_heap_init(heap, (VkDevice)0, &props, &fns, 0, NULL, 0);

   TEST_CHECK(vk_heap_alloc_shared(heap, types, 2, 1000, 256, 1, &a, &type) == VK_SUCCESS);
   TEST_CHECK(type == 0 && a.block != VK_HEAP_LOOSE);
   TEST_CHECK(vk_heap_alloc_shared(heap, types, 2, 1000, 256, 1, &b, &type) == VK_SUCCESS);
   TEST_CHECK(heap->bytes_used == 2000);

   /* Another thread has the blocks */
   retro_atomic_store_release_int(&heap->busy, 1);
   live_before = driver_live;
   TEST_CHECK(vk_heap_alloc_shared(heap, types + 1, 1, 1000, 256, 1, &loose, &type) == VK_SUCCESS);
   TEST_CHECK(loose.block == VK_HEAP_LOOSE && type == 1 && loose.offset == 0);
   TEST_CHECK(loose.mapped == (void*)((size_t)loose.memory << 24));
   TEST_CHECK(driver_live == live_before + 1);
   TEST_CHECK(vk_heap_reserved(heap, 1) == 1000);
   vk_heap_free_shared(heap, &loose);
   TEST_CHECK(driver_live == live_before && vk_heap_reserved(heap, 1) == 0);

   /* a's free waits: what it took is not handed out meanwhile */
   vk_heap_free_shared(heap, &a);
   TEST_CHECK(heap->bytes_used == 2000);
   retro_atomic_store_release_int(&heap->busy, 0);

   /* The blocks taken again: a is freed first, and its place handed out */
   TEST_CHECK(vk_heap_alloc_shared(heap, types, 2, 1000, 256, 1, &c, &type) == VK_SUCCESS);
   TEST_CHECK(c.memory == a.memory && c.offset == a.offset && c.number == a.number);
   TEST_CHECK(heap->bytes_used == 2000);

   vk_heap_free_shared(heap, &b);
   vk_heap_free_shared(heap, &c);
   TEST_CHECK(heap->bytes_used == 0);
   check_blocks(heap);
   vk_heap_shutdown(heap);
   TEST_CHECK(driver_live == 0);
   free(heap);
}

#ifdef VK_HEAP_TEST_THREADS
#define THREADS 4
#define PER_THREAD 200
#ifndef ROUNDS
#define ROUNDS 20000
#endif

typedef struct worker
{
   vk_heap_t *heap;
   vk_heap_alloc_t held[PER_THREAD];
   int in_use[PER_THREAD];
   unsigned long seed;
   unsigned loose;
} worker_t;

static unsigned wrnd(worker_t *w, unsigned n)
{
   w->seed = w->seed * 1103515245ul + 12345ul;
   return (unsigned)((w->seed >> 8) % n);
}

static void worker_run(void *data)
{
   static const unsigned types[3] = { 0, 1, 2 };
   worker_t *w = (worker_t*)data;
   unsigned r;

   for (r = 0; r < ROUNDS; r++)
   {
      const unsigned i = wrnd(w, PER_THREAD);
      unsigned type;

      if (w->in_use[i])
      {
         vk_heap_free_shared(w->heap, &w->held[i]);
         w->in_use[i] = 0;
         continue;
      }
      if (vk_heap_alloc_shared(w->heap, types + wrnd(w, 3), 1,
               1 + wrnd(w, 128 * 1024), 256, (int)wrnd(w, 2), &w->held[i], &type) != VK_SUCCESS)
         continue;
      if (w->held[i].block == VK_HEAP_LOOSE)
         w->loose++;
      w->in_use[i] = 1;
   }
}

/* Threads allocating and freeing at once: what is live at the end never
 * overlaps, and all of it goes back */
static void shared_threads(void)
{
   VkPhysicalDeviceMemoryProperties props;
   vk_heap_t *heap = (vk_heap_t*)malloc(sizeof(*heap));
   worker_t *w = (worker_t*)calloc(THREADS, sizeof(*w));
   sthread_t *t[THREADS];
   unsigned i, j, k, l;
   unsigned loose = 0;
   int bad = 0;

   props_of(&props);
   vk_heap_init(heap, (VkDevice)0, &props, &fns, 0, NULL, 0);
   for (i = 0; i < THREADS; i++)
   {
      w[i].heap = heap;
      w[i].seed = 1000u + i;
      t[i] = sthread_create(worker_run, &w[i]);
   }
   for (i = 0; i < THREADS; i++)
   {
      sthread_join(t[i]);
      loose += w[i].loose;
   }

   for (i = 0; i < THREADS; i++)
      for (j = 0; j < PER_THREAD; j++)
      {
         if (!w[i].in_use[j])
            continue;
         for (k = i; k < THREADS; k++)
            for (l = (k == i) ? j + 1 : 0; l < PER_THREAD; l++)
               if (w[k].in_use[l] && overlaps(&w[i].held[j], &w[k].held[l]))
                  bad++;
      }
   TEST_CHECK(!bad);

   for (i = 0; i < THREADS; i++)
      for (j = 0; j < PER_THREAD; j++)
         if (w[i].in_use[j])
            vk_heap_free_shared(heap, &w[i].held[j]);
   /* What was left while the blocks were busy goes with the next taking */
   {
      static const unsigned type0 = 0;
      vk_heap_alloc_t last;
      unsigned type;
      TEST_CHECK(vk_heap_alloc_shared(heap, &type0, 1, 1, 1, 1, &last, &type) == VK_SUCCESS);
      vk_heap_free_shared(heap, &last);
   }
   TEST_CHECK(heap->bytes_used == 0);
   check_blocks(heap);
   vk_heap_shutdown(heap);
   TEST_CHECK(driver_live == 0);
   TEST_CHECK(vk_heap_reserved(heap, 0) == 0 && vk_heap_reserved(heap, 1) == 0 && vk_heap_reserved(heap, 2) == 0);
   printf("test_vk_heap: %u threads, %u allocations of their own while the blocks were another's\n", THREADS, loose);
   free(w);
   free(heap);
}
#endif

int main(void)
{
   random_and_whole();
   double_free_and_dedicated();
   dedicated_only();
   limit();
   spans();
   shared_one_thread();
#ifdef VK_HEAP_TEST_THREADS
   shared_threads();
#endif

   if (test_failures)
   {
      fprintf(stderr, "test_vk_heap: %d failed\n", test_failures);
      return 1;
   }
   printf("test_vk_heap: allocations apart, aligned, mapped and accounted, kinds apart, big ones dedicated, double frees refused, the limit kept, spans within their array, a warm heap asks the driver for nothing, shared without waiting\n");
   return 0;
}
