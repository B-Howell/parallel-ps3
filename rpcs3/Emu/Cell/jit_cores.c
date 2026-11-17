/* See jit_cores.h. C89. */

#include <stdint.h>

#include <rthreads/retro_waitaddr.h>

#include "jit_cores.h"

void jit_cores_init(jit_cores_t *c, int cores)
{
   retro_atomic_int_init(&c->free, cores);
   retro_atomic_int_init(&c->waiting, 0);
}

int jit_cores_try_take(jit_cores_t *c)
{
   for (;;)
   {
      const int f = retro_atomic_load_acquire_int(&c->free);
      if (f <= 0)
         return 0;
      if (retro_atomic_cas_int(&c->free, f, f - 1))
         return 1;
   }
}

void jit_cores_take(jit_cores_t *c)
{
   if (jit_cores_try_take(c))
      return;

   /* Counted as waiting before the count is looked at again: a give that
    * comes after either sees the waiter, or is seen here */
   retro_atomic_fetch_add_seq_cst_int(&c->waiting, 1);
   for (;;)
   {
      retro_waitaddr_t w;

      if (jit_cores_try_take(c))
         break;

      w.addr = (const void*)&c->free;
      w.old  = 0;
      if (retro_atomic_load_seq_cst_int(&c->free) <= 0)
         retro_waitaddr_wait(&w, 1, -1);
   }
   retro_atomic_fetch_add_int(&c->waiting, -1);
}

void jit_cores_give(jit_cores_t *c)
{
   retro_atomic_fetch_add_seq_cst_int(&c->free, 1);
   if (retro_atomic_load_seq_cst_int(&c->waiting))
      retro_waitaddr_notify_one((const void*)&c->free);
}
