#ifndef JIT_BUMP_H__
#define JIT_BUMP_H__

/* The JIT's memory allocators (JITASM.cpp): a bump pointer over a
 * reserved range, taken with compare-and-swap. A counter word holds the
 * offset of the next free byte in its low 32 bits and how far the range
 * is committed in its high 32; an allocation that would pass the limit
 * sets the limit bit for good, so every later one fails too. Lock-free. */

#include <stdint.h>
#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JIT_BUMP_FAILED (~(uint64_t)0)

/* Takes @size bytes aligned to @align (a power of 2) below @limit (a
 * power of 2 below 2^32, also the failure bit): their offset, or
 * JIT_BUMP_FAILED. *@old_top and *@new_top are the committed top before
 * and the top to commit to (a multiple of @commit_step) */
uint64_t jit_bump_alloc(retro_atomic_64_t *ctr, uint64_t size, uint64_t align,
      uint64_t limit, uint64_t commit_step, uint64_t *old_top, uint64_t *new_top);

/* Raises the committed top to @top if it is below */
void jit_bump_commit(retro_atomic_64_t *ctr, uint64_t top);

/* The offset of the next free byte */
uint64_t jit_bump_used(retro_atomic_64_t *ctr);

/* A bump pointer over [@base, @max): the next @size bytes aligned to
 * @align, or NULL when they do not fit */
unsigned char *jit_bump_ptr_alloc(retro_atomic_ptr_t *pos, unsigned char *max,
      uintptr_t size, uintptr_t align);

#ifdef __cplusplus
}
#endif

#endif
