/* See net_wake.h. C89. */

#include "net_wake.h"

#include <string.h>
#include <net/net_compat.h>
#include <net/net_socket.h>

bool net_wake_init(net_wake_t *w)
{
   struct sockaddr_in addr;
   socklen_t len = sizeof(addr);
   int fd;

   w->fd = -1;
   retro_atomic_int_init(&w->pending, 0);

   if (!network_init())
      return false;

   fd = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
   if (fd < 0)
      return false;

   memset(&addr, 0, sizeof(addr));
   addr.sin_family      = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port        = 0;

   /* Bound to a port the system picks, then connected to it: only its own
    * datagrams come in */
   if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0
         || getsockname(fd, (struct sockaddr*)&addr, &len) != 0
         || connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0
         || !socket_set_block(fd, false))
   {
      socket_close(fd);
      return false;
   }

   w->fd = fd;
   return true;
}

void net_wake_deinit(net_wake_t *w)
{
   if (w->fd >= 0)
      socket_close(w->fd);
   w->fd = -1;
}

void net_wake_poke(net_wake_t *w)
{
   char b = 0;

   if (w->fd < 0)
      return;

   /* One datagram until the poller drains: the first poke since sends */
   if (retro_atomic_exchange_int(&w->pending, 1))
      return;

   send(w->fd, &b, 1, 0);
}

unsigned net_wake_drain(net_wake_t *w)
{
   char b[16];
   unsigned n = 0;

   if (w->fd < 0)
      return 0;

   while (recv(w->fd, b, sizeof(b), 0) > 0)
      n++;

   /* Cleared once read off: a poke made meanwhile sent nothing, but the
    * poller looks at what it waits for after this, so sees what it was
    * for; a poke from here on sends again */
   retro_atomic_exchange_int(&w->pending, 0);

   return n;
}
