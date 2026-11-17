/* The RPCN client's wake-ups (rpcn_signal.c): given once until taken, a
 * timed take that runs out; and under ThreadSanitizer two threads playing
 * ping-pong 100000 times on two of them - every give taken, none lost.
 * C89. */

#include "../../Emu/NP/rpcn_signal.h"
#include "test_common.h"

#include <rthreads/rthreads.h>

#define ROUNDS 100000

static rpcn_signal_t ping, pong;
static retro_atomic_int_t pongs;

static void ponger(void *data)
{
   unsigned i;
   (void)data;
   for (i = 0; i < ROUNDS; i++)
   {
      rpcn_signal_take(&ping);
      retro_atomic_inc_int(&pongs);
      rpcn_signal_give(&pong);
   }
}

int main(void)
{
   sthread_t *t;
   unsigned i;

   /* Given twice, taken once; then a timed take runs out */
   rpcn_signal_init(&ping);
   rpcn_signal_give(&ping);
   rpcn_signal_give(&ping);
   TEST_CHECK(rpcn_signal_take_for(&ping, 0));
   TEST_CHECK(!rpcn_signal_take_for(&ping, 2000));
   rpcn_signal_give(&ping);
   rpcn_signal_take(&ping);

   rpcn_signal_init(&ping);
   rpcn_signal_init(&pong);
   retro_atomic_int_init(&pongs, 0);
   t = sthread_create(ponger, NULL);
   for (i = 0; i < ROUNDS; i++)
   {
      rpcn_signal_give(&ping);
      rpcn_signal_take(&pong);
      if ((unsigned)retro_atomic_load_acquire_int(&pongs) != i + 1)
         TEST_CHECK(0);
   }
   sthread_join(t);

   if (test_failures)
   {
      fprintf(stderr, "test_rpcn_signal: %d failed\n", test_failures);
      return 1;
   }
   printf("test_rpcn_signal: given once until taken, a timed take runs out, %d ping-pongs\n", ROUNDS);
   return 0;
}
