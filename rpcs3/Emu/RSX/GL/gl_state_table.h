#ifndef GL_STATE_TABLE_H__
#define GL_STATE_TABLE_H__

/* What the OpenGL renderer last set of the driver's state, so a call
 * setting it again to the same is not made (glutils/state_tracker.hpp).
 * Fixed tables, made once with the renderer and never grown: no allocation
 * per call, where it was std::unordered_maps hashed and grown on each.
 *
 * - Properties (caps, functions, masks, values), one value each, by their
 *   GLenum (or the tracker's own 0xFFFF00xx keys).
 * - Indexed properties (glEnablei and the like), four values each.
 * - The texture each unit has bound, by target.
 *
 * A key not yet seen reads as changed, and is kept. A table full (more
 * keys than it holds, which the renderer does not use) keeps no more: the
 * call is made each time, as one not cached. C89. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GL_STATE_SLOTS        128 /* a power of two */
#define GL_STATE_INDEXED      32  /* a power of two */
#define GL_STATE_UNITS        48
#define GL_STATE_UNIT_TARGETS 8

typedef struct gl_state_table
{
   uint32_t key[GL_STATE_SLOTS];        /* the key + 1; 0 empty */
   uint64_t val[GL_STATE_SLOTS];
   uint32_t ikey[GL_STATE_INDEXED];
   uint64_t ival[GL_STATE_INDEXED][4];
   uint32_t target[GL_STATE_UNITS][GL_STATE_UNIT_TARGETS]; /* the target + 1; 0 empty */
   uint32_t name[GL_STATE_UNITS][GL_STATE_UNIT_TARGETS];
} gl_state_table_t;

/* 1: already @v; 0: changed (now @v, or not kept as the table is full) */
int gl_state_test_and_set(gl_state_table_t *t, uint32_t key, uint64_t v);

/* The same for value @index (0-3) of an indexed property; a key first
 * seen has its other values 0 */
int gl_state_test_and_set_indexed(gl_state_table_t *t, uint32_t key, unsigned index, uint64_t v);

/* The texture @unit has bound to @target: 0 where none (or not kept) */
uint32_t gl_state_bound_texture(const gl_state_table_t *t, unsigned unit, uint32_t target);

/* 1: @name already bound there; 0: changed (now @name, or not kept) */
int gl_state_bind_texture(gl_state_table_t *t, unsigned unit, uint32_t target, uint32_t name);

/* @name deleted: the first unit found with it bound to @target binds none */
void gl_state_unbind_texture(gl_state_table_t *t, uint32_t target, uint32_t name);

#ifdef __cplusplus
}
#endif

#endif
