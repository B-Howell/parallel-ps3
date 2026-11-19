#include "rw_word.h"

#include <rthreads/retro_waitaddr.h>

/* Sleeps until handed the lock: takes the hand-over bit */
static void rw_wait(retro_atomic_int_t *w)
{
   for (;;)
   {
      retro_waitaddr_t list;
      const unsigned v = rw_value(w);

      if (v >= RW_SIG)
      {
         if (rw_cas(w, v, v - RW_SIG))
            return;
         continue;
      }

      list.addr = w;
      list.old  = (uint32_t)v;
      retro_waitaddr_wait(&list, 1, -1);
   }
}

/* Hands the lock over to one sleeper */
static void rw_signal(retro_atomic_int_t *w)
{
   retro_atomic_fetch_add_int(w, (int)RW_SIG);
   retro_waitaddr_notify_one(w);
}

int rw_lock_shared_slow(retro_atomic_int_t *w, unsigned seen)
{
   unsigned old;

   if (seen >= RW_ERR)
      return 0;

   /* Steal the hand-over */
   if ((seen & RW_SIG) && rw_cas(w, seen, seen - RW_SIG + 1))
      return 1;

   if (rw_try_lock_shared(w))
      return 1;

   /* Queue as a writer, then become a reader */
   old = (unsigned)retro_atomic_fetch_add_int(w, (int)RW_ONE);

   if (old == 0)
   {
      rw_lock_downgrade(w);
      return 1;
   }

   if ((old % RW_SIG) + RW_ONE >= RW_SIG)
      return 0;

   rw_wait(w);
   rw_lock_downgrade(w);
   return 1;
}

int rw_unlock_shared_slow(retro_atomic_int_t *w, unsigned old)
{
   if (old - 1 >= RW_ERR)
      return 0;

   /* The last reader hands over to the waiting writer */
   if ((old - 1) % RW_ONE == 0)
      rw_signal(w);

   return 1;
}

int rw_lock_slow(retro_atomic_int_t *w, unsigned seen)
{
   unsigned old;

   if (seen >= RW_ERR)
      return 0;

   if ((seen & RW_SIG) && rw_cas(w, seen, seen - RW_SIG + RW_ONE))
      return 1;

   old = (unsigned)retro_atomic_fetch_add_int(w, (int)RW_ONE);

   if (old == 0)
      return 1;

   if ((old % RW_SIG) + RW_ONE >= RW_SIG)
      return 0;

   rw_wait(w);
   return 1;
}

int rw_unlock_slow(retro_atomic_int_t *w, unsigned old)
{
   if (old - RW_ONE >= RW_ERR)
      return 0;

   /* Others queued (writers, or readers queued as writers) */
   if (old - RW_ONE)
      rw_signal(w);

   return 1;
}

int rw_lock_upgrade_slow(retro_atomic_int_t *w)
{
   const unsigned old = (unsigned)retro_atomic_fetch_add_int(w, (int)(RW_ONE - 1));

   if ((old % RW_SIG) + RW_ONE - 1 >= RW_SIG)
      return 0;

   if (old % RW_ONE == 1)
      return 1;

   rw_wait(w);
   return 1;
}

void rw_lock_unlock_slow(retro_atomic_int_t *w)
{
   /* Take it and give it back: once taken, nobody held it */
   if (!retro_atomic_fetch_add_int(w, (int)RW_ONE))
   {
      rw_unlock(w);
      return;
   }

   rw_wait(w);
   rw_unlock(w);
}
