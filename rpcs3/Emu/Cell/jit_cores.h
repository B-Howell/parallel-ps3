/* The host cores the JIT's compiles may use at once (g_cfg.core.llvm_threads):
 * a count of the cores free, taken with a compare-and-swap and given back
 * with an add. A compile that finds none free waits on the count until one
 * is given back - woken by the giving, no spin, no sleep. C89. */

#ifndef JIT_CORES_H
#define JIT_CORES_H

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jit_cores
{
   retro_atomic_int_t free;    /* cores not taken */
   retro_atomic_int_t waiting; /* compiles waiting for one */
} jit_cores_t;

void jit_cores_init(jit_cores_t *c, int cores);

/* A core taken, or 0 when none is free */
int jit_cores_try_take(jit_cores_t *c);

/* A core taken, once one is free */
void jit_cores_take(jit_cores_t *c);

/* A core given back, and a compile waiting for one woken */
void jit_cores_give(jit_cores_t *c);

#ifdef __cplusplus
}
#endif

#endif
