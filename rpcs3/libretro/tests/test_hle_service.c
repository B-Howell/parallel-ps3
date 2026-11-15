/* The HLE modules' thread (Emu/Cell/Modules/hle_service.c).
 *
 * - Its first call starts it, from whichever of four threads comes first;
 *   every call of the four runs on it, one at a time.
 * - Stopped, as at a game's unload, and started again by the next call:
 *   3 long rounds and 400 short ones whose four threads start at once,
 *   every call runs on the thread then running. */

#include "../../Emu/Cell/Modules/hle_service.h"
#include "test_common.h"

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define CALLERS 4
#define CALLS   20000

static unsigned calls_each = CALLS;
static retro_atomic_int_t go;

static unsigned plain; /* not atomic: only the thread may add */
static retro_atomic_int_t elsewhere;

static void the_call(void *p)
{
   unsigned was;
   (void)p;
   if (!hle_service_on_thread())
   {
      retro_atomic_fetch_add_int(&elsewhere, 1);
      return;
   }
   was = plain;
   plain = was + 1;
}

static void *caller_main(void *arg)
{
   unsigned n;
   (void)arg;
   /* All four at once, so the first calls race to start the thread */
   while (!retro_atomic_load_acquire_int(&go))
      ;
   for (n = 0; n < calls_each; n++)
      owner_calls_call(hle_service_calls(), the_call, NULL);
   return NULL;
}

int main(void)
{
   int round;
   alarm(300);

   for (round = 0; round < 403; round++)
   {
      pthread_t callers[CALLERS];
      unsigned t;

      plain = 0;
      retro_atomic_int_init(&elsewhere, 0);
      retro_atomic_int_init(&go, 0);
      /* Three long rounds, then many short ones started together */
      calls_each = round < 3 ? CALLS : 50;

      for (t = 0; t < CALLERS; t++)
         TEST_CHECK(pthread_create(&callers[t], NULL, caller_main, NULL) == 0);
      retro_atomic_store_release_int(&go, 1);
      for (t = 0; t < CALLERS; t++)
         pthread_join(callers[t], NULL);

      hle_service_stop();

      TEST_CHECK(plain == CALLERS * calls_each);
      TEST_CHECK(retro_atomic_load_acquire_int(&elsewhere) == 0);
      TEST_CHECK(!hle_service_on_thread());
      if (round >= 3 && round != 402)
         continue;
      printf("test_hle_service: round %d: %u of %d calls run on the thread, %d elsewhere\n",
            round, plain, CALLERS * calls_each, retro_atomic_load_acquire_int(&elsewhere));
   }

   printf("test_hle_service: %s\n", test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
