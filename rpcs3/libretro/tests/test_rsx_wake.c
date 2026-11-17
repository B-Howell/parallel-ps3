/* The renderer's and the RSX offloader's wake word (rsx_wake.c): a wait
 * on a word already moved returns at once; and two threads playing
 * ping-pong 100000 times on two of them, each checking its count before it
 * waits - every signal seen, none lost (a lost one hangs). C89. */

#include "../../Emu/RSX/Common/rsx_wake.h"
#include "test_common.h"

#include <rthreads/rthreads.h>

#define ROUNDS 100000

static rsx_wake_t ping, pong;
static retro_atomic_int_t pings, pongs;

static void wait_for(rsx_wake_t *w, retro_atomic_int_t *count, int target)
{
   for (;;)
   {
      const int seen = rsx_wake_seen(w);
      if (retro_atomic_load_acquire_int(count) >= target)
         return;
      rsx_wake_wait(w, seen);
   }
}

static void ponger(void *data)
{
   int i;
   (void)data;
   for (i = 1; i <= ROUNDS; i++)
   {
      wait_for(&ping, &pings, i);
      retro_atomic_inc_int(&pongs);
      rsx_wake_signal(&pong);
   }
}

int main(void)
{
   sthread_t *t;
   int seen, i;

   /* Moved since it was read: the wait does not block */
   rsx_wake_init(&ping);
   seen = rsx_wake_seen(&ping);
   rsx_wake_signal(&ping);
   rsx_wake_wait(&ping, seen);
   TEST_CHECK(rsx_wake_seen(&ping) == seen + 1);
   TEST_CHECK(retro_atomic_load_acquire_int(&ping.waiting) == 0);

   rsx_wake_init(&ping);
   rsx_wake_init(&pong);
   retro_atomic_int_init(&pings, 0);
   retro_atomic_int_init(&pongs, 0);
   t = sthread_create(ponger, NULL);
   for (i = 1; i <= ROUNDS; i++)
   {
      retro_atomic_inc_int(&pings);
      rsx_wake_signal(&ping);
      wait_for(&pong, &pongs, i);
      if (retro_atomic_load_acquire_int(&pongs) != i)
         TEST_CHECK(0);
   }
   sthread_join(t);

   if (test_failures)
   {
      fprintf(stderr, "test_rsx_wake: %d failed\n", test_failures);
      return 1;
   }
   printf("test_rsx_wake: a moved word is not waited on, %d ping-pongs\n", ROUNDS);
   return 0;
}
