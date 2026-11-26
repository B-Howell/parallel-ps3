/* What the OpenGL renderer last set of the driver's state
 * (rpcs3/Emu/RSX/GL/gl_state_table.c), where it was std::unordered_maps:
 *
 * - a property first seen reads as changed and is kept; the same again
 *   reads as set; another value reads as changed; keys that collide in the
 *   hash (the tracker's own 0xFFFF00xx keys among GLenums) are kept apart;
 * - an indexed property first seen has its other values 0;
 * - a table full keeps no more and reads every new key as changed, every
 *   time (the call made, as one not cached), the keys kept still read set;
 * - a unit's texture first bound reads as changed, the same again as set,
 *   none (0) first seen as set, as the map's default was; unbinding a
 *   deleted name clears the first unit found with it, and only that one. */

#include "../../Emu/RSX/GL/gl_state_table.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

static gl_state_table_t t;

int main(void)
{
   uint32_t k;
   int kept = 0;

   memset(&t, 0, sizeof(t));

   TEST_CHECK(gl_state_test_and_set(&t, 0x0B71, 1) == 0);
   TEST_CHECK(gl_state_test_and_set(&t, 0x0B71, 1) == 1);
   TEST_CHECK(gl_state_test_and_set(&t, 0x0B71, 0) == 0);
   TEST_CHECK(gl_state_test_and_set(&t, 0x0B71, 0) == 1);
   TEST_CHECK(gl_state_test_and_set(&t, 0, 7) == 0 && gl_state_test_and_set(&t, 0, 7) == 1);
   for (k = 0xFFFF0001u; k <= 0xFFFF000Cu; k++)
      TEST_CHECK(gl_state_test_and_set(&t, k, k) == 0);
   for (k = 0xFFFF0001u; k <= 0xFFFF000Cu; k++)
      TEST_CHECK(gl_state_test_and_set(&t, k, k) == 1);

   TEST_CHECK(gl_state_test_and_set_indexed(&t, 0x0BE2, 2, 1) == 0);
   TEST_CHECK(gl_state_test_and_set_indexed(&t, 0x0BE2, 2, 1) == 1);
   TEST_CHECK(gl_state_test_and_set_indexed(&t, 0x0BE2, 0, 0) == 1);
   TEST_CHECK(gl_state_test_and_set_indexed(&t, 0x0BE2, 1, 1) == 0);

   /* Full */
   memset(&t, 0, sizeof(t));
   for (k = 0; k < 1000; k++)
      if (gl_state_test_and_set(&t, 0x8000 + k, 1) == 0 && gl_state_test_and_set(&t, 0x8000 + k, 1) == 1)
         kept++;
   TEST_CHECK(kept == GL_STATE_SLOTS);
   TEST_CHECK(gl_state_test_and_set(&t, 0x9999, 1) == 0 && gl_state_test_and_set(&t, 0x9999, 1) == 0);
   TEST_CHECK(gl_state_test_and_set(&t, 0x8000, 1) == 1);

   /* Textures */
   memset(&t, 0, sizeof(t));
   TEST_CHECK(gl_state_bound_texture(&t, 3, 0x0DE1) == 0);
   TEST_CHECK(gl_state_bind_texture(&t, 3, 0x0DE1, 0) == 1);
   TEST_CHECK(gl_state_bind_texture(&t, 3, 0x0DE1, 42) == 0);
   TEST_CHECK(gl_state_bind_texture(&t, 3, 0x0DE1, 42) == 1);
   TEST_CHECK(gl_state_bound_texture(&t, 3, 0x0DE1) == 42 && gl_state_bound_texture(&t, 3, 0x806F) == 0);
   TEST_CHECK(gl_state_bind_texture(&t, 5, 0x0DE1, 42) == 0);
   gl_state_unbind_texture(&t, 0x0DE1, 42);
   TEST_CHECK(gl_state_bound_texture(&t, 3, 0x0DE1) == 0 && gl_state_bound_texture(&t, 5, 0x0DE1) == 42);
   TEST_CHECK(gl_state_bind_texture(&t, 3, 0x0DE1, 42) == 0);
   TEST_CHECK(gl_state_bind_texture(&t, GL_STATE_UNITS, 0x0DE1, 1) == 0 && gl_state_bound_texture(&t, GL_STATE_UNITS, 0x0DE1) == 0);

   printf("test_gl_state_table: OpenGL state kept in fixed tables: set, changed, first seen, collisions, a full table, texture units; %s\n", test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
