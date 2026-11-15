/* Calls on one object taken in turn (turn_calls.h). Lock-free: an
 * mpsc_stack of handed-over records, retro_waitaddr for the waits. */

#include "turn_calls.h"

#include <rthreads/retro_waitaddr.h>

#include <stddef.h>

typedef struct turn_call_rec
{
   mpsc_stack_node_t node; /* first: the stack's link */
   void (*run)(void *ctx);
   void *ctx;
   retro_atomic_int_t done;
} turn_call_rec_t;

void turn_calls_init(turn_calls_t *t)
{
   mpsc_stack_init(&t->calls);
   retro_atomic_int_init(&t->busy, 0);
}

int turn_calls_busy(turn_calls_t *t)
{
   return retro_atomic_load_acquire_int(&t->busy);
}

/* The turn is this thread's: the calls handed over, then the turn given
 * up. A call handed over after the last look and before the turn went
 * finds the turn taken and is left for whoever has it next: so, the turn
 * given up, the stack is looked at again, and the turn taken back if a
 * call is there and nobody else has taken it. The exchange that gives the
 * turn up is a full barrier, as is the push and the compare-and-swap of a
 * caller: of the push and this look, the later one sees the other. */
static void turn_calls_finish(turn_calls_t *t)
{
   for (;;)
   {
      mpsc_stack_node_t *node;

      while ((node = mpsc_stack_reverse(mpsc_stack_drain(&t->calls))))
      {
         while (node)
         {
            turn_call_rec_t *rec = (turn_call_rec_t *)node;
            node = node->next;

            rec->run(rec->ctx);

            /* The record is the caller's: done is the last of it touched */
            retro_atomic_store_release_int(&rec->done, 1);
            retro_waitaddr_notify_all(&rec->done);
         }
      }

#ifdef TURN_CALLS_TEST_WINDOW
      /* The test widens the window a call can come in by here */
      TURN_CALLS_TEST_WINDOW();
#endif
      (void)retro_atomic_exchange_int(&t->busy, 0);

      if (mpsc_stack_empty(&t->calls) || !retro_atomic_cas_int(&t->busy, 0, 1))
         return;
   }
}

void turn_calls_call(turn_calls_t *t, void (*run)(void *ctx), void *ctx)
{
   turn_call_rec_t rec;

   /* Free: run it here */
   if (retro_atomic_cas_int(&t->busy, 0, 1))
   {
      run(ctx);
      turn_calls_finish(t);
      return;
   }

#ifdef TURN_CALLS_TEST_WINDOW
   /* And here: the turn can be given up before the push */
   TURN_CALLS_TEST_WINDOW();
#endif

   rec.node.next = NULL;
   rec.run       = run;
   rec.ctx       = ctx;
   retro_atomic_int_init(&rec.done, 0);

   mpsc_stack_push(&t->calls, &rec.node);

   /* The thread that had it may have given it up before the push: its
    * look after giving it up then saw the record, or this takes it */
   if (retro_atomic_cas_int(&t->busy, 0, 1))
      turn_calls_finish(t);

   while (!retro_atomic_load_acquire_int(&rec.done))
   {
      retro_waitaddr_t until_done;
      until_done.addr = &rec.done;
      until_done.old  = 0;
      retro_waitaddr_wait(&until_done, 1, -1);
   }
}
