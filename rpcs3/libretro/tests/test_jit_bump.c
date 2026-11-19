/* The JIT's bump allocators (Utilities/jit_bump.c).
 *
 * - Alone: offsets aligned and in order; the committed top raised in
 *   steps and kept; an allocation past the limit fails, and every one
 *   after it too; the pointer form the same within its range.
 * - Two threads, each on a CPU of its own, taking a block each at once
 *   200000 times: never the same block, all aligned, and the counter
 *   ends at the total. */

#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "../../../Utilities/jit_bump.h"
#include "test_common.h"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <retro_atomic.h>

static retro_atomic_64_t ctr;

static void alone(void)
{
   uint64_t o, n, p;
   static unsigned char buf[4096 + 64];
   unsigned char *area = buf + (64 - (uintptr_t)buf % 64) % 64;
   retro_atomic_ptr_t pos;

   p = jit_bump_alloc(&ctr, 10, 16, 0x40000000, 0x200000, &o, &n);
   TEST_CHECK(p == 0 && o == 0 && n == 0x200000);
   jit_bump_commit(&ctr, n);
   p = jit_bump_alloc(&ctr, 100, 64, 0x40000000, 0x200000, &o, &n);
   /* 10 rounded to 16 (the first's end), then to 64 */
   TEST_CHECK(p == 64 && o == 0x200000 && n == 0x200000);
   TEST_CHECK(jit_bump_used(&ctr) == 192);

   /* Past the committed top: a new top to commit, a step on */
   p = jit_bump_alloc(&ctr, 0x200000, 16, 0x40000000, 0x200000, &o, &n);
   TEST_CHECK(p == 192 && o == 0x200000 && n == 0x400000);
   jit_bump_commit(&ctr, n);
   jit_bump_commit(&ctr, 0x200000);
   TEST_CHECK((uint64_t)retro_atomic_load_acquire_64(&ctr) >> 32 == 0x400000);

   /* Past the limit: fails, and so does everything after */
   TEST_CHECK(jit_bump_alloc(&ctr, 0x40000000, 16, 0x40000000, 0x200000, &o, &n) == JIT_BUMP_FAILED);
   TEST_CHECK(jit_bump_alloc(&ctr, 16, 16, 0x40000000, 0x200000, &o, &n) == JIT_BUMP_FAILED);

   retro_atomic_store_release_ptr(&pos, area);
   TEST_CHECK(jit_bump_ptr_alloc(&pos, area + 4096, 100, 64) == area);
   TEST_CHECK(jit_bump_ptr_alloc(&pos, area + 4096, 100, 64) == area + 128);
   TEST_CHECK(jit_bump_ptr_alloc(&pos, area + 4096, 4096, 64) == NULL);
   TEST_CHECK(retro_atomic_load_acquire_ptr(&pos) == (void*)(area + 228));
}

/* Two threads, each on a CPU of its own, taking one block each per round
 * at once (a barrier a round): the two never the same */
#define ROUNDS 200000

static retro_atomic_64_t shared;
static uint64_t got[2][ROUNDS];
static retro_atomic_int_t step_of[2];

static void meet(int me, int step)
{
   retro_atomic_store_release_int(&step_of[me], step);
   while (retro_atomic_load_acquire_int(&step_of[!me]) < step)
      ;
}

static void pin(int cpu)
{
#ifdef __linux__
   cpu_set_t c;
   long n = sysconf(_SC_NPROCESSORS_ONLN);
   if (n < 2)
      return;
   CPU_ZERO(&c);
   CPU_SET(cpu % n, &c);
   pthread_setaffinity_np(pthread_self(), sizeof(c), &c);
#else
   (void)cpu;
#endif
}

static void *taker(void *arg)
{
   const int me = (int)(size_t)arg;
   int r;

   pin(me);
   for (r = 0; r < ROUNDS; r++)
   {
      uint64_t o, n;
      meet(me, r + 1);
      got[me][r] = jit_bump_alloc(&shared, 24, 32, 0x40000000, 0x200000, &o, &n);
      if (n != o)
         jit_bump_commit(&shared, n);
   }

   return NULL;
}

static void together(void)
{
   pthread_t t[2];
   int r, same = 0, unaligned = 0;

   pthread_create(&t[0], NULL, taker, (void*)0);
   pthread_create(&t[1], NULL, taker, (void*)1);
   pthread_join(t[0], NULL);
   pthread_join(t[1], NULL);

   for (r = 0; r < ROUNDS; r++)
   {
      if (got[0][r] == got[1][r])
         same++;
      if (got[0][r] % 32 || got[1][r] % 32)
         unaligned++;
   }

   TEST_CHECK(same == 0);
   TEST_CHECK(unaligned == 0);
   if (same)
      fprintf(stderr, "rounds where both took the same block: %d\n", same);
   TEST_CHECK(jit_bump_used(&shared) == (uint64_t)2 * ROUNDS * 32);
   /* Committed past the end, in steps */
   TEST_CHECK(((uint64_t)retro_atomic_load_acquire_64(&shared) >> 32) >= jit_bump_used(&shared));
}

int main(void)
{
   alarm(300);
   alone();
   together();
   printf("test_jit_bump: %s\n", test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
