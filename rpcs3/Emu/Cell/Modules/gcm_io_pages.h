/* The IO pages a cellGcmSys map or unmap is working on, marked with no
 * lock: 512 pages, a bit each, a 64-bit word's bits taken together with
 * compare-and-swap. A range is marked a word at a time, each word only
 * when none of its pages is marked already; one that is gives back the
 * words marked before it, and the range is refused. C89. */

#ifndef GCM_IO_PAGES_H
#define GCM_IO_PAGES_H

#include <stdint.h>

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GCM_IO_PAGES 0x200u

/* Pages [@io, @io + @count) - those under GCM_IO_PAGES - marked: 1. 0 when
 * one was marked already: nothing changed */
int gcm_io_take(retro_atomic_64_t *pages, uint32_t io, uint32_t count);

/* Pages [@io, @io + @count) no longer marked */
void gcm_io_give(retro_atomic_64_t *pages, uint32_t io, uint32_t count);

int gcm_io_marked(retro_atomic_64_t *pages, uint32_t page);

#ifdef __cplusplus
}
#endif

#endif
