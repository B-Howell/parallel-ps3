/* See vk_interpreter_uploads.h */

#include "vk_interpreter_uploads.h"

#include <string.h>

void vk_interp_uploads_init(struct vk_interp_uploads *u)
{
   memset(u, 0, sizeof(*u));
   /* Nothing is in the buffers yet */
   u->pending = VK_INTERP_ALL;
}

void vk_interp_uploads_changed(struct vk_interp_uploads *u, unsigned dirty)
{
   u->pending |= dirty & VK_INTERP_ALL;
}

unsigned vk_interp_uploads_draw(struct vk_interp_uploads *u,
      const uint32_t vp_config[4], const uint32_t fp_config[2],
      uint64_t vp_heap, uint64_t fp_heap)
{
   unsigned put = u->pending;

   if (memcmp(vp_config, u->vp_config, sizeof(u->vp_config)) != 0
         || vp_heap != u->vp_heap)
      put |= VK_INTERP_VP_DIRTY;

   if (memcmp(fp_config, u->fp_config, sizeof(u->fp_config)) != 0
         || fp_heap != u->fp_heap)
      put |= VK_INTERP_FP_DIRTY;

   if (put & VK_INTERP_VP_DIRTY)
   {
      memcpy(u->vp_config, vp_config, sizeof(u->vp_config));
      u->vp_heap = vp_heap;
   }

   if (put & VK_INTERP_FP_DIRTY)
   {
      memcpy(u->fp_config, fp_config, sizeof(u->fp_config));
      u->fp_heap = fp_heap;
   }

   u->pending = 0;
   return put;
}

int vk_interp_xform_reusable(const struct vk_interp_uploads *u,
      uint64_t epoch, uint64_t seq, uint64_t heap)
{
   return u->xform_valid
      && u->xform_epoch == epoch
      && u->xform_seq   == seq
      && u->xform_heap  == heap;
}

void vk_interp_xform_put(struct vk_interp_uploads *u,
      uint64_t epoch, uint64_t seq, uint64_t heap)
{
   u->xform_valid = 1;
   u->xform_epoch = epoch;
   u->xform_seq   = seq;
   u->xform_heap  = heap;
}
