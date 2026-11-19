#include "init_word.h"

#include <rthreads/retro_waitaddr.h>

unsigned init_word_load(retro_atomic_int_t *w)
{
   return (unsigned)retro_atomic_load_acquire_int(w);
}

int init_word_take(retro_atomic_int_t *w, int forced, unsigned *seen)
{
   for (;;)
   {
      const unsigned v = init_word_load(w);
      unsigned value;

      if (v == 0)
         value = 1;
      else if (forced && (v & INIT_WORD_INIT))
         value = v - (INIT_WORD_INIT - 1);
      else
      {
         *seen = v;
         return 0;
      }

      if (retro_atomic_cas_int(w, (int)v, (int)value))
      {
         *seen = v;
         return 1;
      }
   }
}

static void changed(retro_atomic_int_t *w)
{
   retro_waitaddr_notify_all(w);
}

void init_word_set_init(retro_atomic_int_t *w)
{
   retro_atomic_fetch_add_int(w, (int)(INIT_WORD_INIT - 1));
   changed(w);
}

void init_word_cancel(retro_atomic_int_t *w)
{
   retro_atomic_fetch_sub_int(w, 1);
   changed(w);
}

void init_word_or_init(retro_atomic_int_t *w)
{
   retro_atomic_fetch_or_int(w, (int)INIT_WORD_INIT);
   changed(w);
}

int init_word_access(retro_atomic_int_t *w)
{
   for (;;)
   {
      const unsigned v = init_word_load(w);

      if (!(v & INIT_WORD_INIT))
         return 0;

      if (retro_atomic_cas_int(w, (int)v, (int)(v + 1)))
         return 1;
   }
}

void init_word_release_access(retro_atomic_int_t *w)
{
   /* The count after (the init bit makes it large: as before, a wake
    * only when the bit is clear and at most one holder is left) */
   const unsigned now = (unsigned)retro_atomic_fetch_sub_int(w, 1) - 1;

   if (now <= 1)
      changed(w);
}

void init_word_wait(retro_atomic_int_t *w, unsigned seen)
{
   retro_waitaddr_t list;
   list.addr = w;
   list.old  = (uint32_t)seen;
   retro_waitaddr_wait(&list, 1, -1);
}
