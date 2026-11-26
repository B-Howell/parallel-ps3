/* Ranges by their 32-bit start address in one sorted array
 * (rpcs3/util/addr_map.c), which keeps a guest memory block's allocations
 * (Emu/Memory/vm.cpp): found exactly and as the last at or below an address
 * (vm::block_t::peek), a second insert at an address refused, taken out
 * from the front, middle and back, and checked against a plain array after
 * each of many random steps, in order, each range's two pointers moved
 * with it. C89. */

#include <stdio.h>

#include "../../util/addr_map.h"
#include "test_common.h"

#define N 512

static int      vals[N];
static int      in[N];

/* Addresses 64 KB apart, the top one at the end of the address space */
static uint32_t addr_of(int i) { return (uint32_t)i * 0x10000u + (i == N - 1 ? 0xFFFFu : 0); }

/* Adds @i's range, its pointer put in the room the map gives */
static int add(addr_map_t *m, int i)
{
   const uint32_t at = addr_map_insert(m, addr_of(i), (uint32_t)i);

   if (at == ADDR_MAP_NONE)
      return 0;
   TEST_CHECK(m->e[at].data[0] == NULL && m->e[at].data[1] == NULL && m->addrs[at] == addr_of(i));
   m->e[at].data[0] = &vals[i];
   m->e[at].data[1] = &vals[i];
   return 1;
}

static void check_all(const addr_map_t *m)
{
   uint32_t i, n = 0;
   int      j, last = -1;

   for (i = 0; i + 1 < m->count; i++)
      TEST_CHECK(m->addrs[i] < m->addrs[i + 1]);

   for (j = 0; j < N; j++)
   {
      const uint32_t e = addr_map_find(m, addr_of(j));
      const uint32_t f = addr_map_floor(m, addr_of(j) + 0x8000u);

      TEST_CHECK(in[j] ? (e != ADDR_MAP_NONE && m->e[e].data[0] == &vals[j] && m->e[e].size == (uint32_t)j && m->e[e].data[1] == &vals[j] && m->addrs[e] == addr_of(j))
                       : e == ADDR_MAP_NONE);
      if (in[j])
      {
         last = j;
         n++;
      }
      /* The floor of an address inside slot j is the last present at or before j */
      if (j < N - 1)
         TEST_CHECK(last < 0 ? f == ADDR_MAP_NONE : (f != ADDR_MAP_NONE && m->e[f].data[0] == &vals[last]));
   }
   TEST_CHECK(n == m->count);
}

int main(void)
{
   addr_map_t m = { 0 };
   uint32_t   seed = 777;
   int        i, step;

   TEST_CHECK(addr_map_find(&m, 0) == ADDR_MAP_NONE && addr_map_floor(&m, 0xFFFFFFFFu) == ADDR_MAP_NONE);

   /* Added out of order: odd ones rising, then even ones falling */
   for (i = 1; i < 200; i += 2)
   {
      TEST_CHECK(add(&m, i) == 1);
      in[i] = 1;
   }
   for (i = 198; i >= 0; i -= 2)
   {
      TEST_CHECK(add(&m, i) == 1);
      in[i] = 1;
   }
   TEST_CHECK(m.count == 200);
   TEST_CHECK(addr_map_insert(&m, addr_of(7), 1) == ADDR_MAP_NONE && m.count == 200 && m.e[addr_map_find(&m, addr_of(7))].data[0] == &vals[7]);
   check_all(&m);

   /* Below the first: no floor; far above the last: the last */
   addr_map_erase(&m, addr_map_find(&m, addr_of(0)));
   in[0] = 0;
   TEST_CHECK(addr_map_floor(&m, addr_of(1) - 1) == ADDR_MAP_NONE && m.e[addr_map_floor(&m, addr_of(1))].data[0] == &vals[1]);
   TEST_CHECK(m.e[addr_map_floor(&m, 0xFFFFFFFFu)].data[0] == &vals[199]);

   /* Taken out at the back and in the middle */
   addr_map_erase(&m, addr_map_find(&m, addr_of(199)));
   in[199] = 0;
   addr_map_erase(&m, addr_map_find(&m, addr_of(100)));
   in[100] = 0;
   check_all(&m);

   /* Random inserts and erases, each checked */
   for (step = 0; step < 20000; step++)
   {
      seed = seed * 1103515245u + 12345u;
      i    = (int)((seed >> 8) % N);
      if (in[i])
      {
         addr_map_erase(&m, addr_map_find(&m, addr_of(i)));
         in[i] = 0;
      }
      else
      {
         TEST_CHECK(add(&m, i) == 1);
         in[i] = 1;
      }
      if (step % 101 == 0)
         check_all(&m);
   }
   check_all(&m);

   addr_map_free(&m);
   TEST_CHECK(m.count == 0 && m.cap == 0 && addr_map_find(&m, addr_of(1)) == ADDR_MAP_NONE);
   TEST_CHECK(addr_map_insert(&m, 5, 1) == 0 && addr_map_floor(&m, 6) == 0 && addr_map_floor(&m, 4) == ADDR_MAP_NONE);
   addr_map_free(&m);

   printf("test_addr_map: ranges by address, found exactly and at or below, a second insert refused, taken out, %d random steps checked; %s\n", step, test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
