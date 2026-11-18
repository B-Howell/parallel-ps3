/* Calls run on one owner thread (owner_calls.h). Lock-free: an mpsc_stack
 * of callers' records, retro_waitaddr for the waits. */

#include "owner_calls.h"

#include <rthreads/retro_waitaddr.h>

#include <stddef.h>

void owner_calls_init(owner_calls_t *calls)
{
   mpsc_stack_init(&calls->calls);
   retro_atomic_int_init(&calls->wake, 0);
   retro_atomic_int_init(&calls->callers, 0);
   calls->waker     = NULL;
   calls->waker_ctx = NULL;
}

void owner_calls_set_waker(owner_calls_t *calls, void (*waker)(void *ctx), void *ctx)
{
   calls->waker_ctx = ctx;
   calls->waker     = waker;
}

void owner_calls_wake(owner_calls_t *calls)
{
   retro_atomic_fetch_add_int(&calls->wake, 1);
   retro_waitaddr_notify_all(&calls->wake);

   if (calls->waker)
      calls->waker(calls->waker_ctx);
}

retro_atomic_int_t *owner_calls_wake_word(owner_calls_t *calls)
{
   return &calls->wake;
}

void owner_calls_call(owner_calls_t *calls, void (*run)(void *ctx), void *ctx)
{
   owner_call_t call;

   if (retro_atomic_fetch_add_int(&calls->callers, 1) & OWNER_CALLS_CLOSED)
   {
      /* The owner is gone: nothing else runs its part any more */
      retro_atomic_fetch_sub_int(&calls->callers, 1);
      owner_calls_wake(calls);
      run(ctx);
      return;
   }

   call.node.next = NULL;
   call.run       = run;
   call.ctx       = ctx;
   retro_atomic_int_init(&call.done, 0);

   mpsc_stack_push(&calls->calls, &call.node);
   owner_calls_wake(calls);

   while (!retro_atomic_load_acquire_int(&call.done))
   {
      retro_waitaddr_t until_done;
      until_done.addr  = &call.done;
      until_done.old  = 0;
      retro_waitaddr_wait(&until_done, 1, -1);
   }

   retro_atomic_fetch_sub_int(&calls->callers, 1);
   owner_calls_wake(calls);
}

void owner_calls_run(owner_calls_t *calls)
{
   mpsc_stack_node_t *node = mpsc_stack_reverse(mpsc_stack_drain(&calls->calls));

   while (node)
   {
      owner_call_t *call = (owner_call_t *)node;
      node = node->next;

      call->run(call->ctx);

      /* The record is the caller's: done is the last of it touched */
      retro_atomic_store_release_int(&call->done, 1);
      retro_waitaddr_notify_all(&call->done);
   }
}

void owner_calls_close(owner_calls_t *calls)
{
   retro_atomic_fetch_or_int(&calls->callers, OWNER_CALLS_CLOSED);

   for (;;)
   {
      const int wake = retro_atomic_load_acquire_int(&calls->wake);
      retro_waitaddr_t list;

      owner_calls_run(calls);

      if (!(retro_atomic_load_acquire_int(&calls->callers) & ~OWNER_CALLS_CLOSED))
         break;

      list.addr  = &calls->wake;
      list.old   = (uint32_t)wake;
      retro_waitaddr_wait(&list, 1, -1);
   }
}
