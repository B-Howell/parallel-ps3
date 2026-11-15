/* What the Vulkan shader interpreter puts on the GPU again for a draw
 * (Emu/RSX/VK/vk_interpreter_uploads.c). C89. */

#include "../../Emu/RSX/VK/vk_interpreter_uploads.h"
#include "test_common.h"

static const uint32_t vp_a[4] = { 0, 1, 0xff, 0 };
static const uint32_t vp_b[4] = { 0, 1, 0xff, 1 };
static const uint32_t fp_a[2] = { 0x40, 0x11 };
static const uint32_t fp_b[2] = { 0x40, 0x12 };

int main(void)
{
   struct vk_interp_uploads u;

   vk_interp_uploads_init(&u);

   /* The first interpreted draw puts both in */
   TEST_CHECK(vk_interp_uploads_draw(&u, vp_a, fp_a, 1, 2) == VK_INTERP_ALL);

   /* The same program again, no change: nothing */
   vk_interp_uploads_changed(&u, 0);
   TEST_CHECK(vk_interp_uploads_draw(&u, vp_a, fp_a, 1, 2) == 0);

   /* A vertex program change taken up by a compiled draw in between (its
    * begin saw the change, it never asked the interpreter): the next
    * interpreted draw, whose own begin saw no change, still puts it in */
   vk_interp_uploads_changed(&u, VK_INTERP_VP_DIRTY);
   vk_interp_uploads_changed(&u, 0);
   TEST_CHECK(vk_interp_uploads_draw(&u, vp_a, fp_a, 1, 2) == VK_INTERP_VP_DIRTY);
   TEST_CHECK(vk_interp_uploads_draw(&u, vp_a, fp_a, 1, 2) == 0);

   /* Its words of state, with no program change */
   TEST_CHECK(vk_interp_uploads_draw(&u, vp_b, fp_a, 1, 2) == VK_INTERP_VP_DIRTY);
   TEST_CHECK(vk_interp_uploads_draw(&u, vp_b, fp_b, 1, 2) == VK_INTERP_FP_DIRTY);
   TEST_CHECK(vk_interp_uploads_draw(&u, vp_b, fp_b, 1, 2) == 0);

   /* A new heap under a buffer */
   TEST_CHECK(vk_interp_uploads_draw(&u, vp_b, fp_b, 1, 3) == VK_INTERP_FP_DIRTY);
   TEST_CHECK(vk_interp_uploads_draw(&u, vp_b, fp_b, 4, 3) == VK_INTERP_VP_DIRTY);

   /* Other bits of the change mask are not the interpreter's */
   vk_interp_uploads_changed(&u, 0x100);
   TEST_CHECK(vk_interp_uploads_draw(&u, vp_b, fp_b, 4, 3) == 0);

   /* The whole constant set: nothing on the ring yet */
   TEST_CHECK(!vk_interp_xform_reusable(&u, 0, 0, 9));
   vk_interp_xform_put(&u, 5, 7, 9);
   TEST_CHECK(vk_interp_xform_reusable(&u, 5, 7, 9));
   /* A constant written */
   TEST_CHECK(!vk_interp_xform_reusable(&u, 5, 8, 9));
   /* A frame's ring space given back */
   TEST_CHECK(!vk_interp_xform_reusable(&u, 6, 7, 9));
   /* A new ring */
   TEST_CHECK(!vk_interp_xform_reusable(&u, 5, 7, 10));

   if (test_failures)
   {
      printf("test_vk_interp_uploads: FAIL\n");
      return 1;
   }
   printf("test_vk_interp_uploads: ok\n");
   return 0;
}
