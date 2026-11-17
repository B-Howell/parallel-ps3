/* A word a thread waits on until something it must answer happens - with
 * no lock, no spin and no timeout.
 *
 * The waiter reads the word (rsx_wake_seen), checks whatever it waits for,
 * and if that is not yet so waits for the word to move from what it read
 * (rsx_wake_wait). Whoever makes it so - having made it visible first -
 * moves the word (rsx_wake_signal), and wakes the waiter only when one is
 * marked as waiting: a signal with no one waiting is one add and one load.
 *
 * Nothing is lost between the check and the wait: a signal after the read
 * has moved the word, so the wait returns at once; a signal that saw no
 * waiter came before the mark, so it, too, moved the word first. */

#ifndef RSX_WAKE_H
#define RSX_WAKE_H

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_wake
{
   retro_atomic_int_t word;
   retro_atomic_int_t waiting;
} rsx_wake_t;

void rsx_wake_init(rsx_wake_t *w);

/* For the waiter, before it checks what it waits for */
int rsx_wake_seen(rsx_wake_t *w);

/* For the waiter: returns once the word is no longer @seen */
void rsx_wake_wait(rsx_wake_t *w, int seen);

/* For anyone: the cause made visible, then this */
void rsx_wake_signal(rsx_wake_t *w);

#ifdef __cplusplus
}
#endif

#endif
