/* The HLE modules' own thread, and threads like it (hle_service.h).
 * Lock-free: each started once with a compare-and-swap, its calls on
 * owner_calls, its waits on retro_waitaddr. */

#include "hle_service.h"

#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_waitaddr.h>

#include <stddef.h>

static service_thread_t s_hle;

static void service_thread_main(void *arg)
{
   service_thread_t *svc = (service_thread_t*)arg;
   retro_atomic_store_release_size(&svc->thread_id, sthread_get_current_thread_id());

   for (;;)
   {
      retro_waitaddr_t until;
      const int wake = retro_atomic_load_acquire_int(owner_calls_wake_word(&svc->calls));

      owner_calls_run(&svc->calls);

      if (retro_atomic_load_acquire_int(&svc->stop))
         break;

      until.addr = owner_calls_wake_word(&svc->calls);
      until.old  = (uint32_t)wake;
      retro_waitaddr_wait(&until, 1, -1);
   }

   owner_calls_close(&svc->calls);
}

owner_calls_t *service_thread_calls(service_thread_t *svc)
{
   if (retro_atomic_load_acquire_int(&svc->state) != 2)
   {
      if (retro_atomic_cas_int(&svc->state, 0, 1))
      {
         owner_calls_init(&svc->calls);
         retro_atomic_store_release_int(&svc->stop, 0);
         svc->thread = sthread_create(service_thread_main, svc);
         retro_atomic_store_release_int(&svc->state, 2);
         retro_waitaddr_notify_all(&svc->state);
      }
      else
      {
         /* Another caller is starting it: its calls are ready in a moment */
         while (retro_atomic_load_acquire_int(&svc->state) != 2)
         {
            retro_waitaddr_t until;
            until.addr = &svc->state;
            until.old  = 1;
            retro_waitaddr_wait(&until, 1, -1);
         }
      }
   }

   return &svc->calls;
}

int service_thread_on(service_thread_t *svc)
{
   const size_t id = retro_atomic_load_acquire_size(&svc->thread_id);
   return id && id == sthread_get_current_thread_id();
}

void service_thread_stop(service_thread_t *svc)
{
   if (retro_atomic_load_acquire_int(&svc->state) != 2)
      return;

   retro_atomic_store_release_int(&svc->stop, 1);
   owner_calls_wake(&svc->calls);

   if (svc->thread)
      sthread_join((sthread_t*)svc->thread);

   svc->thread = NULL;
   retro_atomic_store_release_size(&svc->thread_id, 0);
   retro_atomic_store_release_int(&svc->state, 0);
}

owner_calls_t *hle_service_calls(void)
{
   return service_thread_calls(&s_hle);
}

int hle_service_on_thread(void)
{
   return service_thread_on(&s_hle);
}

void hle_service_stop(void)
{
   service_thread_stop(&s_hle);
}
