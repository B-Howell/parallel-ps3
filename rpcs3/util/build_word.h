#ifndef BUILD_WORD_H__
#define BUILD_WORD_H__

/* Something made by the first of a count of starts and taken down by the
 * last stop, kept to one word with no lock: the starts not yet stopped
 * (bits 0-31), whether it is made (bit 32), and whether a caller is making
 * or taking it down (bit 33, the builder).
 *
 * A caller changes the count (build_word_start, _stop); one finding no
 * builder becomes it. The builder asks what to do (build_word_next): make
 * it, take it down, or nothing (then it is the builder no more, unless the
 * count changed meanwhile, when it is asked again). After making or taking
 * down it says so (build_word_done) and asks again. A caller finding a
 * builder only changes the count, for the builder to see: none waits for
 * another. C89. */

#include <stdint.h>
#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BUILD_WORD_MADE    ((uint64_t)1 << 32)
#define BUILD_WORD_BUILDER ((uint64_t)1 << 33)

enum build_word_action
{
   BUILD_WORD_NONE = 0, /* matches the count: no longer the builder */
   BUILD_WORD_MAKE,
   BUILD_WORD_TAKE_DOWN
};

/* 1: the caller is the builder now, to ask build_word_next */
int build_word_start(retro_atomic_64_t *w);

/* 1: the builder now; 0: not; -1: no start to stop (the count unchanged) */
int build_word_stop(retro_atomic_64_t *w);

enum build_word_action build_word_next(retro_atomic_64_t *w);

/* The builder, after making or taking down */
void build_word_done(retro_atomic_64_t *w);

/* A count restored as saved: made where it is not 0, no builder */
void build_word_restore(retro_atomic_64_t *w, uint32_t count);

uint32_t build_word_count(retro_atomic_64_t *w);

#ifdef __cplusplus
}
#endif

#endif
