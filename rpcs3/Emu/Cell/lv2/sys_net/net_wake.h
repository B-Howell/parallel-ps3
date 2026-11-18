/* A wake-up for a thread blocked in poll(): a UDP socket on the loopback,
 * connected to itself. The thread polls its fd (POLLIN) along with its
 * sockets, untimed; another thread pokes it to have it look again - its set
 * changed, a call was handed over, or it is to stop. Pokes before the
 * thread drains are one datagram. Any platform with BSD sockets or
 * Winsock. C89. */

#ifndef NET_WAKE_H
#define NET_WAKE_H

#include <boolean.h>
#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct net_wake
{
   int fd;                     /* -1 when it could not be made */
   retro_atomic_int_t pending; /* a datagram sent and not drained */
} net_wake_t;

bool net_wake_init(net_wake_t *w);
void net_wake_deinit(net_wake_t *w);

/* Any thread: the poller wakes (one datagram, if none is pending) */
void net_wake_poke(net_wake_t *w);

/* The poller, once its poll returned: the datagrams read off. It looks at
 * what it waits for after this; pokes from here on wake it again. Gives
 * how many were read */
unsigned net_wake_drain(net_wake_t *w);

#ifdef __cplusplus
}
#endif

#endif
