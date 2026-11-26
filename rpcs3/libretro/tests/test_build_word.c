/* Made by the first start, taken down by the last stop, with no lock
 * (rpcs3/util/build_word.c), where sys_io's config queue was made and taken
 * down behind a mutex the game's threads waited on:
 *
 * - on one thread, as a game calls them: the start makes it, a second start
 *   and its stop do nothing, the last stop takes it down; a stop with no
 *   start is refused and changes nothing; a count restored as saved is made
 *   where it is not 0;
 * - on four threads each starting and stopping 50000 times at once: it is
 *   made and taken down only in turn, never two builders at once, and at the end
 *   it is made exactly when the count is not 0, with no builder left. */

#include "../../util/build_word.h"
#include "test_common.h"

#include <pthread.h>
#include <stdio.h>

#define THREADS 4
#define ROUNDS  50000

static retro_atomic_64_t w;
static retro_atomic_int_t made;      /* 1 while made */
static retro_atomic_int_t building;  /* builders at work */
static retro_atomic_int_t bad_turn;  /* made twice, or taken down unmade */
static retro_atomic_int_t overlaps;
static retro_atomic_int_t builds;

static void build(void)
{
   enum build_word_action a;

   while ((a = build_word_next(&w)) != BUILD_WORD_NONE)
   {
      volatile int spin;

      if (retro_atomic_fetch_add_int(&building, 1) != 0)
         retro_atomic_inc_int(&overlaps);

      if (a == BUILD_WORD_MAKE)
      {
         if (retro_atomic_exchange_int(&made, 1) != 0)
            retro_atomic_inc_int(&bad_turn);
      }
      else if (retro_atomic_exchange_int(&made, 0) != 1)
         retro_atomic_inc_int(&bad_turn);

      /* A while making or taking down, as the lv2 calls take */
      for (spin = 0; spin < 300; spin++) {}
      retro_atomic_inc_int(&builds);

      retro_atomic_fetch_sub_int(&building, 1);
      build_word_done(&w);
   }
}

static void *caller(void *arg)
{
   const int keep = (int)(size_t)arg;
   int r;

   /* A start and its stop, over and over: the count crosses 0 often */
   for (r = 0; r < ROUNDS; r++)
   {
      if (build_word_start(&w) == 1)
         build();
      if (build_word_stop(&w) == 1)
         build();
   }

   /* Then leaves @keep starts unstopped, for the end state */
   for (r = 0; r < keep; r++)
      if (build_word_start(&w) == 1)
         build();
   return NULL;
}

int main(void)
{
   pthread_t t[THREADS];
   int i, expect = 0;

   /* One thread, as a game calls them */
   build_word_restore(&w, 0);
   TEST_CHECK(build_word_stop(&w) == -1 && build_word_count(&w) == 0);
   TEST_CHECK(build_word_start(&w) == 1);
   TEST_CHECK(build_word_next(&w) == BUILD_WORD_MAKE);
   build_word_done(&w);
   TEST_CHECK(build_word_next(&w) == BUILD_WORD_NONE);
   TEST_CHECK(build_word_start(&w) == 1 && build_word_next(&w) == BUILD_WORD_NONE);
   TEST_CHECK(build_word_stop(&w) == 1 && build_word_next(&w) == BUILD_WORD_NONE);
   TEST_CHECK(build_word_stop(&w) == 1 && build_word_next(&w) == BUILD_WORD_TAKE_DOWN);
   build_word_done(&w);
   TEST_CHECK(build_word_next(&w) == BUILD_WORD_NONE && build_word_count(&w) == 0);
   TEST_CHECK(((uint64_t)retro_atomic_load_acquire_64(&w) & (BUILD_WORD_MADE | BUILD_WORD_BUILDER)) == 0);
   build_word_restore(&w, 2);
   TEST_CHECK(build_word_count(&w) == 2 && ((uint64_t)retro_atomic_load_acquire_64(&w) & BUILD_WORD_MADE));

   /* Four threads at once */
   build_word_restore(&w, 0);
   for (i = 0; i < THREADS; i++)
   {
      pthread_create(&t[i], NULL, caller, (void*)(size_t)i);
      expect += i;
   }
   for (i = 0; i < THREADS; i++)
      pthread_join(t[i], NULL);

   TEST_CHECK(retro_atomic_load_acquire_int(&overlaps) == 0);
   TEST_CHECK(retro_atomic_load_acquire_int(&bad_turn) == 0);
   TEST_CHECK(build_word_count(&w) == (uint32_t)expect);
   TEST_CHECK(retro_atomic_load_acquire_int(&made) == (expect != 0));
   TEST_CHECK(!!((uint64_t)retro_atomic_load_acquire_64(&w) & BUILD_WORD_MADE) == (expect != 0));
   TEST_CHECK(!((uint64_t)retro_atomic_load_acquire_64(&w) & BUILD_WORD_BUILDER));

   printf("test_build_word: made by the first start, taken down by the last stop, %d starts and stops on 4 threads: %d builds, %d overlaps, %d out of turn; %s\n",
         THREADS * ROUNDS, retro_atomic_load_acquire_int(&builds), retro_atomic_load_acquire_int(&overlaps),
         retro_atomic_load_acquire_int(&bad_turn), test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
