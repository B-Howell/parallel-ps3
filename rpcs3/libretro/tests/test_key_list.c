/* A short list of pointers by a 64-bit key (rpcs3/util/key_list.c), where
 * an image's views were an std::unordered_map:
 *
 * - found by key, a key not there found as none, a value 0 key and keys
 *   differing only in their top half kept apart;
 * - the first four kept in the list itself (nothing allocated), more on
 *   the heap, all still found after it grows and grows again;
 * - moved: every entry found in the new list, the old one empty and
 *   usable; freed: empty, and usable again. */

#include "../../util/key_list.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

static int vals[64];

int main(void)
{
   key_list_t a, b;
   int i;

   memset(&a, 0, sizeof(a));
   TEST_CHECK(key_list_find(&a, 1) == NULL && key_list_at(&a, 0) == NULL);
   TEST_CHECK(key_list_add(&a, 0, &vals[0]) == 1);
   TEST_CHECK(key_list_add(&a, (uint64_t)1 << 32, &vals[1]) == 1);
   TEST_CHECK(key_list_add(&a, 1, &vals[2]) == 1);
   TEST_CHECK(key_list_find(&a, 0) == &vals[0] && key_list_find(&a, (uint64_t)1 << 32) == &vals[1] && key_list_find(&a, 1) == &vals[2]);
   TEST_CHECK(key_list_find(&a, 2) == NULL);

   /* Four inline, then the heap */
   TEST_CHECK(key_list_add(&a, 3, &vals[3]) == 1);
   TEST_CHECK(a.count == 4 && a.cap == 0);
   TEST_CHECK(key_list_add(&a, 4, &vals[4]) == 1);
   TEST_CHECK(a.cap == 8);
   for (i = 5; i < 40; i++)
      TEST_CHECK(key_list_add(&a, (uint64_t)i, &vals[i]) == 1);
   TEST_CHECK(a.count == 40 && a.cap == 64);
   TEST_CHECK(key_list_find(&a, 0) == &vals[0] && key_list_find(&a, (uint64_t)1 << 32) == &vals[1] && key_list_find(&a, 1) == &vals[2]);
   for (i = 3; i < 40; i++)
      TEST_CHECK(key_list_find(&a, (uint64_t)i) == &vals[i]);
   for (i = 0; i < 40; i++)
      TEST_CHECK(key_list_at(&a, (uint32_t)i) != NULL);
   TEST_CHECK(key_list_at(&a, 40) == NULL);

   /* Moved */
   key_list_move(&b, &a);
   TEST_CHECK(a.count == 0 && a.cap == 0 && key_list_find(&a, 7) == NULL);
   TEST_CHECK(b.count == 40 && key_list_find(&b, 7) == &vals[7] && key_list_find(&b, 39) == &vals[39]);
   TEST_CHECK(key_list_add(&a, 9, &vals[9]) == 1 && key_list_find(&a, 9) == &vals[9]);

   /* An inline list moved (into one freed first: a move overwrites) */
   key_list_free(&b);
   key_list_move(&b, &a);
   TEST_CHECK(b.count == 1 && b.cap == 0 && key_list_find(&b, 9) == &vals[9]);

   /* Freed */
   key_list_free(&b);
   TEST_CHECK(b.count == 0 && key_list_find(&b, 9) == NULL);
   TEST_CHECK(key_list_add(&b, 5, &vals[5]) == 1 && key_list_find(&b, 5) == &vals[5]);
   key_list_free(&b);

   printf("test_key_list: pointers by key, four kept in the list, more on the heap, moved and freed; %s\n", test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
