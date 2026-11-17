/* A word waited on until something to answer happens (rsx_wake.h) */

#include "rsx_wake.h"

#include <stdint.h>
#include <rthreads/retro_waitaddr.h>

void rsx_wake_init(rsx_wake_t *w)
{
   retro_atomic_int_init(&w->word, 0);
   retro_atomic_int_init(&w->waiting, 0);
}

int rsx_wake_seen(rsx_wake_t *w)
{
   return retro_atomic_load_seq_cst_int(&w->word);
}

void rsx_wake_wait(rsx_wake_t *w, int seen)
{
   retro_waitaddr_t on;

   on.addr = &w->word;
   on.old  = (uint32_t)seen;

   /* Marked, then waited on: a signal that misses the mark moved the word
    * before it looked, and the wait sees it moved */
   retro_atomic_fetch_add_seq_cst_int(&w->waiting, 1);

   while ((uint32_t)retro_atomic_load_seq_cst_int(&w->word) == on.old)
      retro_waitaddr_wait(&on, 1, -1);

   retro_atomic_fetch_add_seq_cst_int(&w->waiting, -1);
}

void rsx_wake_signal(rsx_wake_t *w)
{
   retro_atomic_fetch_add_seq_cst_int(&w->word, 1);

   if (retro_atomic_load_seq_cst_int(&w->waiting))
      retro_waitaddr_notify_all(&w->word);
}
