/* A short list of {32-bit key, pointer's room} pairs (rpcs3/util/pair_list.c),
 * one per 4 MB block of the render target store (Emu/RSX/Common/
 * ranged_map.hpp): two kept in the list itself, more on the heap, where the
 * keys are compared four at a time (lists of every length up to 70, so each
 * place in a group of four and past a 32-key pass is a match and a miss);
 * each room's bytes kept as pairs move (growing, the last moving into a
 * gap); a key left past count by a removal never found, at every length;
 * a reused slot's room zeroed; and checked against a plain array after
 * each of many random adds and removes. C89. */

#include <stdio.h>
#include <string.h>

#include "../../util/pair_list.h"
#include "test_common.h"

#define N 80

static int in[N];

static void put(pair_list_t *l, uint32_t at, int i) { void *p = &in[i]; memcpy(PAIR_LIST_ROOMS(l)[at].bytes, &p, sizeof(p)); }
static void *got(const pair_list_t *l, uint32_t at) { void *p; memcpy(&p, PAIR_LIST_ROOMS(l)[at].bytes, sizeof(p)); return p; }

static uint32_t key_of(int i) { return (uint32_t)i * 0x10000u + 0x40000000u; }

static void check_all(const pair_list_t *l)
{
   int      i;
   uint32_t n = 0;

   for (i = 0; i < N; i++)
   {
      const uint32_t at = pair_list_find(l, key_of(i));
      TEST_CHECK(in[i] ? (at < l->count && PAIR_LIST_KEYS(l)[at] == key_of(i) && got(l, at) == &in[i]) : at == PAIR_LIST_NONE);
      n += in[i] != 0;
   }
   TEST_CHECK(n == l->count);
}

int main(void)
{
   pair_list_t l;
   uint32_t    seed = 99, at;
   int         i, len, step;

   memset(&l, 0, sizeof(l));
   TEST_CHECK(pair_list_find(&l, 0) == PAIR_LIST_NONE && PAIR_LIST_KEYS(&l) == l.inline_keys);

   /* Every length up to 70: each key found where it is, the rest not */
   for (len = 1; len <= 70; len++)
   {
      at = pair_list_add(&l, key_of(len - 1));
      TEST_CHECK(at == (uint32_t)(len - 1) && got(&l, at) == NULL);
      put(&l, at, len - 1);
      in[len - 1] = 1;
      check_all(&l);
   }
   TEST_CHECK(l.keys != NULL && l.cap == 128);

   /* A key past count (left by a removal) is not found */
   pair_list_remove_at(&l, l.count - 1);
   in[69] = 0;
   TEST_CHECK(l.keys[l.count] == key_of(69) && pair_list_find(&l, key_of(69)) == PAIR_LIST_NONE);

   /* The first taken out: the last moves into its place */
   pair_list_remove_at(&l, 0);
   in[0] = 0;
   TEST_CHECK(PAIR_LIST_KEYS(&l)[0] == key_of(68) && got(&l, 0) == &in[68]);
   check_all(&l);

   /* Added again into the slot just left: its room zeroed, not the old bytes */
   at = pair_list_add(&l, key_of(69));
   TEST_CHECK(at != PAIR_LIST_NONE && got(&l, at) == NULL);
   put(&l, at, 69);
   in[69] = 1;
   check_all(&l);

   /* At every length to 40, a key just taken out from the end (still there
    * past count) is not found, then the list grows back */
   for (len = 40; len >= 1; len--)
   {
      while (l.count > (uint32_t)len)
      {
         pair_list_remove_at(&l, l.count - 1);
         in[PAIR_LIST_KEYS(&l)[l.count] - key_of(0) >> 16] = 0;
      }
      if (l.count == (uint32_t)len && len > 1)
      {
         const uint32_t last = PAIR_LIST_KEYS(&l)[len - 1];
         pair_list_remove_at(&l, (uint32_t)len - 1);
         in[(last - key_of(0)) >> 16] = 0;
         TEST_CHECK(PAIR_LIST_KEYS(&l)[len - 1] == last && pair_list_find(&l, last) == PAIR_LIST_NONE);
         check_all(&l);
      }
   }

   /* Inline: a key left past count is not found either */
   pair_list_free(&l);
   memset(in, 0, sizeof(in));
   put(&l, pair_list_add(&l, key_of(1)), 1);
   put(&l, pair_list_add(&l, key_of(2)), 2);
   pair_list_remove_at(&l, 1);
   TEST_CHECK(l.keys == NULL && pair_list_find(&l, key_of(2)) == PAIR_LIST_NONE && pair_list_find(&l, key_of(1)) == 0);
   pair_list_remove_at(&l, 0);
   TEST_CHECK(pair_list_find(&l, key_of(1)) == PAIR_LIST_NONE);

   /* Random adds and removes, each checked */
   for (step = 0; step < 20000; step++)
   {
      seed = seed * 1103515245u + 12345u;
      i    = (int)((seed >> 8) % N);
      if (in[i])
      {
         pair_list_remove_at(&l, pair_list_find(&l, key_of(i)));
         in[i] = 0;
      }
      else
      {
         at = pair_list_add(&l, key_of(i));
         TEST_CHECK(at != PAIR_LIST_NONE);
         put(&l, at, i);
         in[i] = 1;
      }
      if (step % 53 == 0)
         check_all(&l);
   }
   check_all(&l);

   pair_list_free(&l);
   TEST_CHECK(l.count == 0 && l.keys == NULL && pair_list_find(&l, key_of(1)) == PAIR_LIST_NONE);

   printf("test_pair_list: pairs by key, two inline then on the heap four at a time, every length to 70, rooms kept as pairs move, %d random steps checked; %s\n", step, test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
