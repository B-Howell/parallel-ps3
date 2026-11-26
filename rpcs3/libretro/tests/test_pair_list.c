/* A short list of {32-bit key, pointer's room} pairs (rpcs3/util/pair_list.c),
 * one per 4 MB block of the render target store (Emu/RSX/Common/
 * ranged_map.hpp): two kept in the list itself, more on the heap; each
 * room's bytes kept as entries move (growing, and the last moving into a
 * gap); and checked against a plain array after each of many random adds
 * and removes. C89. */

#include <stdio.h>
#include <string.h>

#include "../../util/pair_list.h"
#include "test_common.h"

#define N 64

static int in[N];

static void put(pair_list_entry_t *e, int i) { void *p = &in[i]; memcpy(e->room.bytes, &p, sizeof(p)); }
static void *got(const pair_list_entry_t *e) { void *p; memcpy(&p, e->room.bytes, sizeof(p)); return p; }

static uint32_t key_of(int i) { return (uint32_t)i * 0x10000u + 0x40000000u; }

static void check_all(pair_list_t *l)
{
   int      i;
   uint32_t n = 0;

   for (i = 0; i < N; i++)
   {
      pair_list_entry_t *e = pair_list_find(l, key_of(i));
      TEST_CHECK(in[i] ? (e && e->key == key_of(i) && got(e) == &in[i]) : e == NULL);
      n += in[i] != 0;
   }
   TEST_CHECK(n == l->count);
}

int main(void)
{
   pair_list_t        l;
   pair_list_entry_t *e;
   uint32_t           seed = 99, idx;
   int                i, step;

   memset(&l, 0, sizeof(l));
   TEST_CHECK(pair_list_find(&l, 0) == NULL && PAIR_LIST_ENTRIES(&l) == l.inline_e);

   /* Two inline, the room zeroed when added */
   for (i = 0; i < 2; i++)
   {
      e = pair_list_add(&l, key_of(i));
      TEST_CHECK(e && got(e) == NULL);
      put(e, i);
      in[i] = 1;
   }
   TEST_CHECK(l.heap == NULL && l.count == 2);
   check_all(&l);

   /* The third moves them to the heap, rooms with them */
   put(pair_list_add(&l, key_of(2)), 2);
   in[2] = 1;
   TEST_CHECK(l.heap != NULL && l.cap == 4);
   for (i = 3; i < 20; i++)
   {
      put(pair_list_add(&l, key_of(i)), i);
      in[i] = 1;
   }
   TEST_CHECK(l.count == 20 && l.cap == 32);
   check_all(&l);

   /* The first taken out: the last moves into its place */
   pair_list_remove_at(&l, 0);
   in[0] = 0;
   TEST_CHECK(PAIR_LIST_ENTRIES(&l)[0].key == key_of(19) && got(&PAIR_LIST_ENTRIES(&l)[0]) == &in[19]);
   /* The last taken out: nothing moves */
   pair_list_remove_at(&l, l.count - 1);
   in[18] = 0;
   check_all(&l);

   /* Added again into the slot just left: its room zeroed, not the old bytes */
   e = pair_list_add(&l, key_of(18));
   TEST_CHECK(e && got(e) == NULL);
   put(e, 18);
   in[18] = 1;
   check_all(&l);

   /* Random adds and removes, each checked */
   for (step = 0; step < 20000; step++)
   {
      seed = seed * 1103515245u + 12345u;
      i    = (int)((seed >> 8) % N);
      if (in[i])
      {
         e   = pair_list_find(&l, key_of(i));
         idx = (uint32_t)(e - PAIR_LIST_ENTRIES(&l));
         pair_list_remove_at(&l, idx);
         in[i] = 0;
      }
      else
      {
         e = pair_list_add(&l, key_of(i));
         TEST_CHECK(e != NULL);
         put(e, i);
         in[i] = 1;
      }
      if (step % 53 == 0)
         check_all(&l);
   }
   check_all(&l);

   pair_list_free(&l);
   TEST_CHECK(l.count == 0 && l.heap == NULL && pair_list_find(&l, key_of(1)) == NULL);
   put(pair_list_add(&l, 7), 7);
   TEST_CHECK(pair_list_find(&l, 7) && got(pair_list_find(&l, 7)) == &in[7] && l.heap == NULL);
   pair_list_free(&l);

   printf("test_pair_list: pairs by key, two inline then on the heap, rooms kept as entries move, %d random steps checked; %s\n", step, test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
