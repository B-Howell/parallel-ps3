/* A wake-up for the thread waiting on it, given once until taken: what the
 * RPCN client's threads wake each other with (once a binary semaphore).
 * A word given with an exchange and waited on (retro_waitaddr): no lock, no
 * spin. C89. */

#ifndef RPCN_SIGNAL_H
#define RPCN_SIGNAL_H

#include <stdint.h>

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rpcn_signal
{
   retro_atomic_int_t given;
} rpcn_signal_t;

void rpcn_signal_init(rpcn_signal_t *s);

/* Given (again while given: still once), the waiting thread woken */
void rpcn_signal_give(rpcn_signal_t *s);

/* Taken, once given */
void rpcn_signal_take(rpcn_signal_t *s);

/* Taken, or 0 when not given within @us microseconds (a network timer) -
 * or sooner, now and then: the caller looks at its timers again */
int rpcn_signal_take_for(rpcn_signal_t *s, int64_t us);

#ifdef __cplusplus
}
#endif

#endif
