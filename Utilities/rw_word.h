#ifndef RW_WORD_H__
#define RW_WORD_H__

/* A reader/writer lock in one 32-bit word (shared_mutex, mutex.h): the
 * low 14 bits count readers, each writer (holding or queued) adds
 * RW_ONE, RW_SIG is a hand-over to one sleeper, the top bit flags a
 * broken state. The fast paths are one compare-and-swap or one atomic
 * add, inline here; the slow paths (rw_word.c) sleep on the word with
 * retro_waitaddr until handed it, with no spinning. */

#include <stdint.h>
#include <retro_inline.h>
#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RW_ONE (1u << 14)
#define RW_SIG (1u << 30)
#define RW_ERR (1u << 31)

static INLINE unsigned rw_value(retro_atomic_int_t *w)
{
   return (unsigned)retro_atomic_load_acquire_int(w);
}

static INLINE int rw_cas(retro_atomic_int_t *w, unsigned old, unsigned value)
{
   return retro_atomic_cas_int(w, (int)old, (int)value);
}

/* The slow paths: 0 on a broken state (an underflow or overflow) */
int rw_lock_shared_slow(retro_atomic_int_t *w, unsigned seen);
int rw_unlock_shared_slow(retro_atomic_int_t *w, unsigned old);
int rw_lock_slow(retro_atomic_int_t *w, unsigned seen);
int rw_unlock_slow(retro_atomic_int_t *w, unsigned old);
int rw_lock_upgrade_slow(retro_atomic_int_t *w);
void rw_lock_unlock_slow(retro_atomic_int_t *w);

static INLINE int rw_try_lock_shared(retro_atomic_int_t *w)
{
   const unsigned v = rw_value(w);
   return v < RW_ONE - 1 && rw_cas(w, v, v + 1);
}

static INLINE int rw_lock_shared(retro_atomic_int_t *w)
{
   const unsigned v = rw_value(w);
   if (v < RW_ONE - 1 && rw_cas(w, v, v + 1))
      return 1;
   return rw_lock_shared_slow(w, v);
}

static INLINE int rw_unlock_shared(retro_atomic_int_t *w)
{
   /* Unconditional decrement (can result in a broken state) */
   const unsigned v = (unsigned)retro_atomic_fetch_sub_int(w, 1);
   if (v < RW_ONE)
      return 1;
   return rw_unlock_shared_slow(w, v);
}

static INLINE int rw_try_lock(retro_atomic_int_t *w)
{
   return rw_cas(w, 0, RW_ONE);
}

static INLINE int rw_lock(retro_atomic_int_t *w)
{
   if (rw_cas(w, 0, RW_ONE))
      return 1;
   return rw_lock_slow(w, rw_value(w));
}

static INLINE int rw_unlock(retro_atomic_int_t *w)
{
   const unsigned v = (unsigned)retro_atomic_fetch_sub_int(w, (int)RW_ONE);
   if (v == RW_ONE)
      return 1;
   return rw_unlock_slow(w, v);
}

/* A single reader made the writer, other writers ignored */
static INLINE int rw_try_lock_upgrade(retro_atomic_int_t *w)
{
   const unsigned v = rw_value(w);
   return (v + RW_ONE - 1) % RW_ONE == 0 && rw_cas(w, v, v + RW_ONE - 1);
}

static INLINE int rw_lock_upgrade(retro_atomic_int_t *w)
{
   return rw_try_lock_upgrade(w) || rw_lock_upgrade_slow(w);
}

/* The writer made a reader (can result in a broken state) */
static INLINE void rw_lock_downgrade(retro_atomic_int_t *w)
{
   retro_atomic_fetch_sub_int(w, (int)(RW_ONE - 1));
}

/* Waits until the lock could be taken, without keeping it */
static INLINE void rw_lock_unlock(retro_atomic_int_t *w)
{
   if (rw_value(w) != 0)
      rw_lock_unlock_slow(w);
}

#ifdef __cplusplus
}
#endif

#endif
