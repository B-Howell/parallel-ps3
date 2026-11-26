/* Pointers by a 64-bit key in an open-addressed table (rpcs3/util/ptr_table.c),
 * which keeps the fonts and images an overlay draws (VKOverlays.h,
 * GLOverlays.h): found, added past a growth, taken out by key and by slot
 * (the entries after shifting back), spread through the table as it grows, and checked against a plain array
 * after each of many random steps. C89. */

#include <stdio.h>
#include <stdlib.h>

#include "../../util/ptr_table.h"
#include "test_common.h"

#define N 600

static int      vals[N];
static int      in[N];

/* Keys a fixed stride apart (as allocations are) and some far ones */
static uint64_t key_of(int i)
{
   return (i & 1) ? (((uint64_t)0x7F12 << 32) | 0x00401000) + (uint64_t)i * 0x90
                  : ((uint64_t)i << 40) | 0x10;
}

static void check_all(const ptr_table_t *t)
{
   uint32_t i, n = 0;

   for (i = 0; i < N; i++)
      TEST_CHECK(ptr_table_find(t, key_of((int)i)) == (in[i] ? &vals[i] : NULL));
   for (i = 0; i < t->cap; i++)
      n += ptr_table_slot(t, i) != NULL;
   TEST_CHECK(n == t->count && t->count * 2 <= t->cap);
}

int main(void)
{
   ptr_table_t t = { 0 };
   uint32_t    i, n, seed = 12345;
   int         step;

   TEST_CHECK(ptr_table_find(&t, 1) == NULL && ptr_table_remove(&t, 1) == NULL);
   TEST_CHECK(ptr_table_slot(&t, 0) == NULL && ptr_table_remove_at(&t, 0) == NULL);
   TEST_CHECK(ptr_table_add(&t, 1, NULL) == 0 && t.count == 0);

   /* Added past several growths */
   for (i = 0; i < 300; i++)
   {
      TEST_CHECK(ptr_table_add(&t, key_of((int)i), &vals[i]) == 1);
      in[i] = 1;
   }
   TEST_CHECK(t.count == 300 && t.cap == 1024);
   check_all(&t);

   /* Spread through the whole table as it grew, not packed into a run */
   for (i = 3 * t.cap / 4, n = 0; i < t.cap; i++)
      n += ptr_table_slot(&t, i) != NULL;
   TEST_CHECK(n >= 30);

   /* Taken out by key: every third, then one not there */
   for (i = 0; i < 300; i += 3)
   {
      TEST_CHECK(ptr_table_remove(&t, key_of((int)i)) == &vals[i]);
      in[i] = 0;
   }
   TEST_CHECK(ptr_table_remove(&t, key_of(0)) == NULL && ptr_table_remove(&t, key_of(450)) == NULL);
   check_all(&t);

   /* Taken out by slot, the odd ones: the same slot looked at again */
   for (i = 0; i < t.cap;)
   {
      const int *v = (const int*)ptr_table_slot(&t, i);
      if (!v || ((v - vals) & 1) == 0)
      {
         i++;
         continue;
      }
      TEST_CHECK(ptr_table_remove_at(&t, i) == v);
      in[v - vals] = 0;
   }
   for (i = 0, n = 0; i < N; i++)
      n += in[i];
   TEST_CHECK(n == t.count);
   check_all(&t);

   /* Random adds and removes, each checked */
   for (step = 0; step < 20000; step++)
   {
      seed = seed * 1103515245u + 12345u;
      i    = (seed >> 8) % N;
      if (in[i])
      {
         TEST_CHECK(ptr_table_remove(&t, key_of((int)i)) == &vals[i]);
         in[i] = 0;
      }
      else
      {
         TEST_CHECK(ptr_table_add(&t, key_of((int)i), &vals[i]) == 1);
         in[i] = 1;
      }
      if (step % 97 == 0)
         check_all(&t);
   }
   check_all(&t);

   ptr_table_free(&t);
   TEST_CHECK(t.count == 0 && t.cap == 0 && ptr_table_find(&t, key_of(1)) == NULL);
   TEST_CHECK(ptr_table_add(&t, key_of(1), &vals[1]) == 1 && ptr_table_find(&t, key_of(1)) == &vals[1]);
   ptr_table_free(&t);

   printf("test_ptr_table: pointers by key, grown, taken out by key and by slot, %d random steps checked; %s\n", step, test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
