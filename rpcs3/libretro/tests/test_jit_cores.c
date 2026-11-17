/* The cores the JIT's compiles may use at once (jit_cores.c): taken and
 * given back with no lock; and under ThreadSanitizer eight threads taking
 * three cores at once, some only trying - never more than three held, each
 * waiter woken by a give. C89. */

#include "../../Emu/Cell/jit_cores.h"
#include "test_common.h"

#include <rthreads/rthreads.h>
#include <sched.h>

#define CORES   3
#define THREADS 8
#define ROUNDS  20000

static jit_cores_t cores;
static retro_atomic_int_t held;
static retro_atomic_int_t most;
static retro_atomic_int_t errors;

static void hold(void)
{
   const int now = retro_atomic_fetch_add_int(&held, 1) + 1;
   if (now > CORES)
      retro_atomic_inc_int(&errors);
   if (now > retro_atomic_load_acquire_int(&most))
      retro_atomic_store_release_int(&most, now);
   sched_yield();
   retro_atomic_fetch_add_int(&held, -1);
}

static void worker(void *data)
{
   const unsigned me = (unsigned)(size_t)data;
   unsigned r;

   for (r = 0; r < ROUNDS; r++)
   {
      if ((r + me) % 4 == 0)
      {
         if (!jit_cores_try_take(&cores))
            continue;
      }
      else
         jit_cores_take(&cores);
      hold();
      jit_cores_give(&cores);
   }
}

int main(void)
{
   sthread_t *t[THREADS];
   unsigned i;

   /* Alone: taken to none, then given back */
   jit_cores_init(&cores, 2);
   TEST_CHECK(jit_cores_try_take(&cores) && jit_cores_try_take(&cores) && !jit_cores_try_take(&cores));
   jit_cores_give(&cores);
   jit_cores_take(&cores);
   TEST_CHECK(!jit_cores_try_take(&cores));
   jit_cores_give(&cores);
   jit_cores_give(&cores);
   TEST_CHECK(retro_atomic_load_acquire_int(&cores.free) == 2);

   jit_cores_init(&cores, CORES);
   retro_atomic_int_init(&held, 0);
   retro_atomic_int_init(&most, 0);
   retro_atomic_int_init(&errors, 0);
   for (i = 0; i < THREADS; i++)
      t[i] = sthread_create(worker, (void*)(size_t)i);
   for (i = 0; i < THREADS; i++)
      sthread_join(t[i]);

   TEST_CHECK(retro_atomic_load_acquire_int(&errors) == 0);
   TEST_CHECK(retro_atomic_load_acquire_int(&cores.free) == CORES && retro_atomic_load_acquire_int(&cores.waiting) == 0);
   if (test_failures)
   {
      fprintf(stderr, "test_jit_cores: %d failed\n", test_failures);
      return 1;
   }
   printf("test_jit_cores: %d threads on %d cores: at most %d held, every waiter woken\n",
         THREADS, CORES, retro_atomic_load_acquire_int(&most));
   return 0;
}
