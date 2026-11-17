/* Another thread holding the renderer still (rsx_hold.h) */

#include "rsx_hold.h"

#include <stdint.h>
#include <rthreads/retro_waitaddr.h>

void rsx_hold_init(rsx_hold_t *h)
{
   retro_atomic_int_init(&h->asked, 0);
   retro_atomic_int_init(&h->ack, 0);
}

void rsx_hold_ask(rsx_hold_t *h)
{
   retro_waitaddr_t on;

   retro_atomic_fetch_add_seq_cst_int(&h->asked, 1);

   on.addr = &h->ack;

   for (;;)
   {
      on.old = (uint32_t)retro_atomic_load_seq_cst_int(&h->ack);
      if (on.old & 3u)
         return;
      retro_waitaddr_wait(&on, 1, -1);
   }
}

void rsx_hold_release(rsx_hold_t *h, rsx_wake_t *wake)
{
   retro_atomic_fetch_add_seq_cst_int(&h->asked, -1);
   rsx_wake_signal(wake);
}

int rsx_hold_asked(rsx_hold_t *h)
{
   return retro_atomic_load_seq_cst_int(&h->asked) != 0;
}

void rsx_hold_park(rsx_hold_t *h, rsx_wake_t *wake,
      int (*keep)(void *ctx), void (*serve)(void *ctx), void *ctx)
{
   do
   {
      /* Held, and the asking threads told: a new count with bit 0 set */
      for (;;)
      {
         const int v = retro_atomic_load_seq_cst_int(&h->ack);
         if (retro_atomic_cas_int(&h->ack, v, (int)(((unsigned)v + 4u) | 1u)))
            break;
      }
      retro_waitaddr_notify_all(&h->ack);

      for (;;)
      {
         const int seen = rsx_wake_seen(wake);

         if (!rsx_hold_asked(h) || (keep && !keep(ctx)))
            break;

         if (serve)
            serve(ctx);

         rsx_wake_wait(wake, seen);
      }

      retro_atomic_fetch_and_int(&h->ack, ~1);

      /* A thread that asked as the last let go may have seen it held, and
       * gone on: held again for it */
   } while (rsx_hold_asked(h) && !(keep && !keep(ctx)));
}

void rsx_hold_gone(rsx_hold_t *h)
{
   retro_atomic_fetch_or_int(&h->ack, 2);
   retro_waitaddr_notify_all(&h->ack);
}
