/* See rpcn_signal.h. C89. */

#include <rthreads/retro_waitaddr.h>

#include "rpcn_signal.h"

void rpcn_signal_init(rpcn_signal_t *s)
{
   retro_atomic_int_init(&s->given, 0);
}

void rpcn_signal_give(rpcn_signal_t *s)
{
   /* Given, then the waiter woken: one that looks after sees it given */
   if (!retro_atomic_exchange_int(&s->given, 1))
      retro_waitaddr_notify_one((const void*)&s->given);
}

void rpcn_signal_take(rpcn_signal_t *s)
{
   while (!retro_atomic_exchange_int(&s->given, 0))
   {
      retro_waitaddr_t w;
      w.addr = (const void*)&s->given;
      w.old  = 0;
      retro_waitaddr_wait(&w, 1, -1);
   }
}

int rpcn_signal_take_for(rpcn_signal_t *s, int64_t us)
{
   retro_waitaddr_t w;

   if (retro_atomic_exchange_int(&s->given, 0))
      return 1;
   w.addr = (const void*)&s->given;
   w.old  = 0;
   retro_waitaddr_wait(&w, 1, us);
   return retro_atomic_exchange_int(&s->given, 0) != 0;
}
