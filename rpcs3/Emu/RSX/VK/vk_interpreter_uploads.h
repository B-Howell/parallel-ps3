/* What the shader interpreter has to put on the GPU again for a draw, and
 * what it can take as it is.
 *
 * The interpreter reads the game's shader programs from two instruction
 * buffers. What was last put in them stays good until the program or its
 * few words of state change, so a draw puts in again only what changed.
 * A change can be taken up by a compiled draw in between (in "async with
 * interpreter" most draws are compiled once their shaders are), so the
 * changes are gathered from every draw, not only the interpreter's.
 *
 * A program that reads the transform constants by index (the interpreter,
 * or a compiled one that does) takes the whole set. The set last put on the
 * ring is good while no constant was written and no frame's ring space was
 * given back since, so a switch between such programs puts nothing in.
 *
 * All of it is the RSX thread's. */

#ifndef VK_INTERPRETER_UPLOADS_H
#define VK_INTERPRETER_UPLOADS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Program changes, as the RSX's pipeline state bits give them */
#define VK_INTERP_VP_DIRTY 1u
#define VK_INTERP_FP_DIRTY 2u
#define VK_INTERP_ALL      (VK_INTERP_VP_DIRTY | VK_INTERP_FP_DIRTY)

struct vk_interp_uploads
{
   /* Owed to the instruction buffers */
   unsigned pending;

   /* What they hold */
   uint32_t vp_config[4];
   uint32_t fp_config[2];
   uint64_t vp_heap;
   uint64_t fp_heap;

   /* The whole set of transform constants last put on the ring */
   int      xform_valid;
   uint64_t xform_epoch;
   uint64_t xform_seq;
   uint64_t xform_heap;
};

void vk_interp_uploads_init(struct vk_interp_uploads *u);

/* A draw begins with these program changes not yet taken up */
void vk_interp_uploads_changed(struct vk_interp_uploads *u, unsigned dirty);

/* An interpreted draw: which buffers to put in again (VK_INTERP_* bits).
 * The config words and heaps are what the draw has; once the caller has
 * put in what was asked, the buffers are taken to hold them. */
unsigned vk_interp_uploads_draw(struct vk_interp_uploads *u,
      const uint32_t vp_config[4], const uint32_t fp_config[2],
      uint64_t vp_heap, uint64_t fp_heap);

/* Whether the whole constant set on the ring can be taken as it is */
int vk_interp_xform_reusable(const struct vk_interp_uploads *u,
      uint64_t epoch, uint64_t seq, uint64_t heap);

/* The whole set was just put on the ring */
void vk_interp_xform_put(struct vk_interp_uploads *u,
      uint64_t epoch, uint64_t seq, uint64_t heap);

#ifdef __cplusplus
}
#endif

#endif
