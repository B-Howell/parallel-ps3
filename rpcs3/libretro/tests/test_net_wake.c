/* The poll() wake-up (net_wake.c): a poke from another thread wakes a
 * thread blocked in poll on it, pokes before a drain are one datagram,
 * and a poke after the drain wakes it again. The poll here is given 10
 * seconds only so that a broken wake fails the test rather than hang it.
 * C89. */

#include "../../Emu/Cell/lv2/sys_net/net_wake.h"
#include "test_common.h"

#include <net/net_compat.h>
#include <net/net_socket.h>
#include <rthreads/rthreads.h>

static net_wake_t wake;
static retro_atomic_int_t woke;

static int readable(int ms)
{
   struct pollfd p;
   p.fd      = wake.fd;
   p.events  = POLLIN;
   p.revents = 0;
   return socket_poll(&p, 1, ms) == 1 && (p.revents & POLLIN);
}

static void poller(void *data)
{
   (void)data;
   if (readable(10000))
      retro_atomic_store_release_int(&woke, 1);
}

int main(void)
{
   sthread_t *t;
   int i;

   TEST_CHECK(net_wake_init(&wake));
   TEST_CHECK(wake.fd >= 0);

   /* Nothing to read at first */
   TEST_CHECK(!readable(0));

   /* A thread in poll, woken by a poke */
   retro_atomic_int_init(&woke, 0);
   t = sthread_create(poller, NULL);
   net_wake_poke(&wake);
   sthread_join(t);
   TEST_CHECK(retro_atomic_load_acquire_int(&woke) == 1);

   /* Many pokes before the drain: one datagram */
   for (i = 0; i < 1000; i++)
      net_wake_poke(&wake);
   TEST_CHECK(net_wake_drain(&wake) == 1);
   TEST_CHECK(!readable(0));

   /* A poke after the drain wakes it again */
   net_wake_poke(&wake);
   TEST_CHECK(readable(10000));
   TEST_CHECK(net_wake_drain(&wake) == 1);
   TEST_CHECK(!readable(0));

   net_wake_deinit(&wake);

   if (test_failures)
      return 1;
   printf("test_net_wake: a poke wakes poll, pokes before a drain are one datagram\n");
   return 0;
}
