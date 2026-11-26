/* The Vulkan renderer's samplers by their key
 * (rpcs3/Emu/RSX/VK/vkutils/vk_sampler_table.c), where they were two
 * std::unordered_maps:
 *
 * - found by both halves of their key, a border colour of 0 or not; a key
 *   there already is not replaced;
 * - taken out in any order, from the middle of a run of colliding keys and
 *   from runs wrapping past the table's end, every one left is still found
 *   and none taken out is (the run after one taken out shifted back);
 * - a full table keeps no more; collecting with no pick takes them all. */

#include "../../Emu/RSX/VK/vkutils/vk_sampler_table.h"
#include "test_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static vk_sampler_table_t t;
static int objs[VK_SAMPLER_SLOTS];
static int taken[VK_SAMPLER_SLOTS];
static int n_keys;

static uint64_t key_of(int i) { return (uint64_t)i * 0x10001u + 7u; }

static int pick_odd(void *obj, void *ctx)
{
   (void)ctx;
   return (int)(((int*)obj - objs) & 1);
}

static void take(void *obj, void *ctx)
{
   (void)ctx;
   taken[(int*)obj - objs]++;
}

static int all_found(void)
{
   int i, ok = 1;
   for (i = 0; i < n_keys; i++)
   {
      void *f = vk_sampler_table_find(&t, key_of(i), (uint64_t)(i % 3));
      if (taken[i] ? f != NULL : f != &objs[i])
         ok = 0;
   }
   return ok;
}

int main(void)
{
   int i, kept = 0, n;

   memset(&t, 0, sizeof(t));
   TEST_CHECK(vk_sampler_table_insert(&t, 1, 0, &objs[0]) == 1);
   TEST_CHECK(vk_sampler_table_insert(&t, 1, 5, &objs[1]) == 1);
   TEST_CHECK(vk_sampler_table_find(&t, 1, 0) == &objs[0] && vk_sampler_table_find(&t, 1, 5) == &objs[1]);
   TEST_CHECK(vk_sampler_table_insert(&t, 1, 0, &objs[2]) == 0 && vk_sampler_table_find(&t, 1, 0) == &objs[0]);
   TEST_CHECK(vk_sampler_table_find(&t, 2, 0) == NULL);

   /* Three quarters full, then taken out at random, checking all each time */
   memset(&t, 0, sizeof(t));
   n_keys = VK_SAMPLER_SLOTS * 3 / 4;
   for (i = 0; i < n_keys; i++)
      kept += vk_sampler_table_insert(&t, key_of(i), (uint64_t)(i % 3), &objs[i]);
   TEST_CHECK(kept == n_keys && t.count == (uint32_t)n_keys);
   TEST_CHECK(all_found());

   vk_sampler_table_collect(&t, pick_odd, take, NULL);
   for (i = 0, n = 0; i < n_keys; i++)
      n += taken[i];
   TEST_CHECK(n == n_keys / 2 && t.count == (uint32_t)(n_keys - n));
   for (i = 0; i < n_keys; i++)
      if (taken[i] > 1)
         TEST_CHECK(!"taken twice");
   TEST_CHECK(all_found());

   /* Full: keeps no more */
   for (i = 0; i < n_keys; i++)
      if (taken[i])
      {
         taken[i] = 0;
         vk_sampler_table_insert(&t, key_of(i), (uint64_t)(i % 3), &objs[i]);
      }
   for (i = n_keys; i < VK_SAMPLER_SLOTS; i++)
      vk_sampler_table_insert(&t, key_of(i), (uint64_t)(i % 3), &objs[i]);
   TEST_CHECK(t.count == VK_SAMPLER_SLOTS - 1);
   TEST_CHECK(vk_sampler_table_insert(&t, 0xFFFFFFFFu, 9, &objs[0]) == 0);

   /* All taken */
   memset(taken, 0, sizeof(taken));
   vk_sampler_table_collect(&t, NULL, take, NULL);
   TEST_CHECK(t.count == 0);
   for (i = 0, n = 0; i < VK_SAMPLER_SLOTS; i++)
      n += taken[i];
   TEST_CHECK(n == VK_SAMPLER_SLOTS - 1);

   /* A run wrapping past the end: 40 keys whose home is one of the last 4
    * slots (found by where each lands in an empty table), taken out every
    * other one, the rest still found */
   {
      static vk_sampler_table_t scratch;
      uint64_t keys[40], k;
      int found = 0, j;

      for (k = 0; found < 40; k++)
      {
         memset(&scratch, 0, sizeof(scratch));
         vk_sampler_table_insert(&scratch, k, 0, &objs[0]);
         for (j = VK_SAMPLER_SLOTS - 4; j < VK_SAMPLER_SLOTS; j++)
            if (scratch.obj[j])
               keys[found++] = k;
      }

      memset(&t, 0, sizeof(t));
      memset(taken, 0, sizeof(taken));
      for (j = 0; j < 40; j++)
         TEST_CHECK(vk_sampler_table_insert(&t, keys[j], 0, &objs[j]) == 1);
      TEST_CHECK(t.obj[0] != NULL); /* the run wrapped */
      vk_sampler_table_collect(&t, pick_odd, take, NULL);
      TEST_CHECK(t.count == 20);
      for (j = 0; j < 40; j++)
         TEST_CHECK(vk_sampler_table_find(&t, keys[j], 0) == ((j & 1) ? NULL : &objs[j]));
   }

   printf("test_vk_sampler_table: Vulkan samplers by key in a fixed table: found, not replaced, taken out from runs, full, all taken; %s\n", test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
