/* The HLE modules' own thread (hle_service.h). Lock-free: started once
 * with a compare-and-swap, its calls on owner_calls, its waits on
 * retro_waitaddr. */

#include "hle_service.h"

#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_waitaddr.h>

#include <stddef.h>

static owner_calls_t s_calls;
static sthread_t *s_thread;
static retro_atomic_size_t s_thread_id; /* the thread's, once it runs */
static retro_atomic_int_t s_state;      /* 0 not started, 1 starting, 2 running */
static retro_atomic_int_t s_stop;

static void hle_service_main(void *arg)
{
   (void)arg;
   retro_atomic_store_release_size(&s_thread_id, sthread_get_current_thread_id());

   for (;;)
   {
      retro_waitaddr_t until;
      const int wake = retro_atomic_load_acquire_int(owner_calls_wake_word(&s_calls));

      owner_calls_run(&s_calls);

      if (retro_atomic_load_acquire_int(&s_stop))
         break;

      until.addr = owner_calls_wake_word(&s_calls);
      until.old  = (uint32_t)wake;
      retro_waitaddr_wait(&until, 1, -1);
   }

   owner_calls_close(&s_calls);
}

owner_calls_t *hle_service_calls(void)
{
   if (retro_atomic_load_acquire_int(&s_state) != 2)
   {
      if (retro_atomic_cas_int(&s_state, 0, 1))
      {
         owner_calls_init(&s_calls);
         retro_atomic_store_release_int(&s_stop, 0);
         s_thread = sthread_create(hle_service_main, NULL);
         retro_atomic_store_release_int(&s_state, 2);
         retro_waitaddr_notify_all(&s_state);
      }
      else
      {
         /* Another caller is starting it: its calls are ready in a moment */
         while (retro_atomic_load_acquire_int(&s_state) != 2)
         {
            retro_waitaddr_t until;
            until.addr = &s_state;
            until.old  = 1;
            retro_waitaddr_wait(&until, 1, -1);
         }
      }
   }

   return &s_calls;
}

int hle_service_on_thread(void)
{
   const size_t id = retro_atomic_load_acquire_size(&s_thread_id);
   return id && id == sthread_get_current_thread_id();
}

void hle_service_stop(void)
{
   if (retro_atomic_load_acquire_int(&s_state) != 2)
      return;

   retro_atomic_store_release_int(&s_stop, 1);
   owner_calls_wake(&s_calls);

   if (s_thread)
      sthread_join(s_thread);

   s_thread = NULL;
   retro_atomic_store_release_size(&s_thread_id, 0);
   retro_atomic_store_release_int(&s_state, 0);
}
