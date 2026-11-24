/* suspend_all's guard, with no lock (cpu_guard.h). C89 */

#include "cpu_guard.h"

#include <rthreads/retro_waitaddr.h>

/* Until *word is not old, counted in *waiters for the one changing it to
 * wake */
static void wait_change(retro_atomic_int_t *word, retro_atomic_int_t *waiters, int old)
{
   retro_waitaddr_t w;

   /* A read-modify-write, a full barrier: one changing the word after this
    * sees the count, and one that changed it before is seen below */
   retro_atomic_inc_int(waiters);
   w.addr = word;
   w.old  = (uint32_t)old;
   while (retro_atomic_load_acquire_int(word) == old)
      retro_waitaddr_wait(&w, 1, -1);
   retro_atomic_dec_int(waiters);
}

static void changed(retro_atomic_int_t *word, retro_atomic_int_t *waiters)
{
   /* The change before the look at the count */
   retro_atomic_thread_fence_seq_cst();
   if (retro_atomic_load_acquire_int(waiters))
      retro_waitaddr_notify_all(word);
}

void cpu_guard_suspend_begin(cpu_guard_t *g)
{
   for (;;)
   {
      const int e = retro_atomic_load_acquire_int(&g->epoch);

      if (e & 1)
         wait_change(&g->epoch, &g->epoch_waiters, e);
      else if (retro_atomic_cas_int(&g->epoch, e, e + 1))
         break;
   }

   /* Odd before the list is read */
   retro_atomic_thread_fence_seq_cst();
}

void cpu_guard_suspend_end(cpu_guard_t *g)
{
   retro_atomic_inc_int(&g->epoch);
   changed(&g->epoch, &g->epoch_waiters);
}

void cpu_guard_wait_suspend(cpu_guard_t *g)
{
   int e;

   /* The caller's store to the list before the look at the epoch */
   retro_atomic_thread_fence_seq_cst();
   e = retro_atomic_load_acquire_int(&g->epoch);
   if (e & 1)
      wait_change(&g->epoch, &g->epoch_waiters, e);
}

void cpu_guard_read_begin(cpu_guard_t *g)
{
   /* A full barrier: counted before the list is read */
   retro_atomic_inc_int(&g->readers);
}

void cpu_guard_read_end(cpu_guard_t *g)
{
   /* The last out wakes a thread leaving */
   if (retro_atomic_fetch_sub_int(&g->readers, 1) == 1)
      changed(&g->readers, &g->readers_waiters);
}

void cpu_guard_retire(cpu_guard_t *g)
{
   int r;

   cpu_guard_wait_suspend(g);

   /* Until none walks: a walker coming after this can't read the pointer,
    * taken out before the fence in cpu_guard_wait_suspend */
   while ((r = retro_atomic_load_acquire_int(&g->readers)) != 0)
      wait_change(&g->readers, &g->readers_waiters, r);
}
