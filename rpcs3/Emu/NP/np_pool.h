/* The memory pool the NP libraries hand guest buffers out of (sceNpInit's
 * pool), as offsets into it, from any thread with no lock and no thread
 * waiting for another. C89.
 *
 * The pool is cut in 4-byte granules, as the allocator it replaces
 * aligned to. Three bitmaps of 64-bit words: the granules in use, and the
 * first and last granule of each allocation. An allocation takes the
 * lowest run of free granules that fits, claiming its words one at a time
 * with compare-and-swap; a thread that finds part of the run taken
 * meanwhile gives back what it claimed and looks again. A free clears the
 * allocation's last granule, then its granules. Nothing allocates after
 * np_pool_init. */

#ifndef NP_POOL_H
#define NP_POOL_H

#include <stddef.h>
#include <stdint.h>

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_POOL_GRANULE 4u

typedef struct np_pool
{
   uint32_t           granules;
   uint32_t           words;
   retro_atomic_64_t *used;
   retro_atomic_64_t *first;
   retro_atomic_64_t *last;
   retro_atomic_int_t avail;     /* bytes */
   retro_atomic_int_t max_used;  /* bytes, the most ever in use */
} np_pool_t;

/* size bytes, rounded down to a granule. 0 when the bitmaps cannot be had.
 * A zeroed pool (or one after np_pool_deinit) is an empty one of size 0 */
int np_pool_init(np_pool_t *pool, uint32_t size);
void np_pool_deinit(np_pool_t *pool);

/* The lowest run of free granules that holds size bytes: its offset in
 * *offset. 0 for a size of 0 or when no run is free */
int np_pool_alloc(np_pool_t *pool, uint32_t size, uint32_t *offset);

/* The allocation at offset, freed. 0 when no allocation starts there */
int np_pool_free(np_pool_t *pool, uint32_t offset);

/* The allocation at offset made new_size bytes or less, its tail freed. 0
 * when no allocation starts there or it is not that large */
int np_pool_shrink(np_pool_t *pool, uint32_t offset, uint32_t new_size);

/* Bytes of the allocation at offset, 0 when none starts there. For a save
 * state, with nothing else using the pool */
uint32_t np_pool_size_of(const np_pool_t *pool, uint32_t offset);

/* The next allocation at offset or after: 1 with its offset and size, 0
 * when there is none. For a save state */
int np_pool_next(const np_pool_t *pool, uint32_t from, uint32_t *offset,
      uint32_t *size);

/* Marks offset..offset+size as an allocation, as a save state had it. 0
 * when it does not fit or overlaps one */
int np_pool_mark(np_pool_t *pool, uint32_t offset, uint32_t size);

/* Bytes in the pool, free, and the most ever in use */
uint32_t np_pool_size(const np_pool_t *pool);
uint32_t np_pool_avail(const np_pool_t *pool);
uint32_t np_pool_max_used(const np_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif
