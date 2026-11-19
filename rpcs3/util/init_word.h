#ifndef INIT_WORD_H__
#define INIT_WORD_H__

/* The word behind stx::init_mutex (init_mutex.hpp): a resource's state
 * for the three ways it is used - initialised (the top bit), and a count
 * of holders, the one initialising or finalising it among them. Changed
 * with compare-and-swap; a waiter sleeps on the word with retro_waitaddr
 * until a change wakes it. Lock-free. */

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INIT_WORD_INIT 0x80000000u

/* The init lock taken: from 0 (nothing), or with @forced from
 * initialised (made the init holder, keeping the others' access).
 * 1 if taken, with *@seen the word before; 0 if not, with *@seen it */
int init_word_take(retro_atomic_int_t *w, int forced, unsigned *seen);

/* The init lock given up, initialised now */
void init_word_set_init(retro_atomic_int_t *w);

/* The init lock given up, not initialised */
void init_word_cancel(retro_atomic_int_t *w);

/* Marked initialised (a reset lock's set_init) */
void init_word_or_init(retro_atomic_int_t *w);

/* An access holder added, only while initialised: whether it was */
int init_word_access(retro_atomic_int_t *w);

/* An access holder gone: wakes the waiters when at most one is left */
void init_word_release_access(retro_atomic_int_t *w);

unsigned init_word_load(retro_atomic_int_t *w);

/* Sleeps while the word is @seen (may return early) */
void init_word_wait(retro_atomic_int_t *w, unsigned seen);

#ifdef __cplusplus
}
#endif

#endif
