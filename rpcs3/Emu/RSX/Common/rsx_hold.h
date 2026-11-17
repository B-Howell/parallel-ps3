/* Another thread holding the renderer still (rsx::eng_lock) - with no lock,
 * no spin and no timeout.
 *
 * A thread asks (rsx_hold_ask) and waits until the renderer, between two
 * pieces of its own work, sees it asked and holds (rsx_hold_park), or is
 * gone for good (rsx_hold_gone). The renderer stays held until every
 * thread that asked has let go (rsx_hold_release) or it is told to stop
 * holding (@keep says no); while held it answers what it must (@serve) -
 * faults the asking threads may be waiting on - and waits on its wake word
 * otherwise, which a release moves. */

#ifndef RSX_HOLD_H
#define RSX_HOLD_H

#include <retro_atomic.h>

#include "rsx_wake.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_hold
{
   /* The threads that asked and have not let go */
   retro_atomic_int_t asked;
   /* Bit 0 while held, bit 1 once gone, the rest counting holds */
   retro_atomic_int_t ack;
} rsx_hold_t;

void rsx_hold_init(rsx_hold_t *h);

/* For any thread but the renderer: returns once it is held, or gone */
void rsx_hold_ask(rsx_hold_t *h);

/* For a thread that asked: the renderer, held, looks again */
void rsx_hold_release(rsx_hold_t *h, rsx_wake_t *wake);

/* Whether a thread asked: one load */
int rsx_hold_asked(rsx_hold_t *h);

/* For the renderer, seeing it asked: held until no thread asks or @keep
 * says no, calling @serve between waits on @wake. @keep, @serve may be
 * NULL */
void rsx_hold_park(rsx_hold_t *h, rsx_wake_t *wake,
      int (*keep)(void *ctx), void (*serve)(void *ctx), void *ctx);

/* For the renderer, leaving for good */
void rsx_hold_gone(rsx_hold_t *h);

#ifdef __cplusplus
}
#endif

#endif
