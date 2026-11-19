/* The word behind stx::init_mutex (rpcs3/util/init_word.c).
 *
 * - Alone: the init lock taken from nothing and refused while held or
 *   once initialised; access only while initialised, counted; a forced
 *   take of an initialised word keeps the access holders; cancel and
 *   set_init as before.
 * - A thread waiting to finalise (the forced take, then waiting for the
 *   access holders to leave) is woken when the last one leaves.
 * - Four threads taking and giving back access 200000 times each at
 *   once: the count ends where it began. */

#include "../../util/init_word.h"
#include "test_common.h"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

static retro_atomic_int_t w;

static void alone(void)
{
   unsigned seen = 7;

   TEST_CHECK(!init_word_access(&w));
   TEST_CHECK(init_word_take(&w, 0, &seen) && seen == 0);
   TEST_CHECK(!init_word_take(&w, 0, &seen) && seen == 1);
   TEST_CHECK(!init_word_take(&w, 1, &seen));
   init_word_set_init(&w);
   TEST_CHECK(init_word_load(&w) == INIT_WORD_INIT);
   /* Initialised: no plain init lock, access granted */
   TEST_CHECK(!init_word_take(&w, 0, &seen) && seen == INIT_WORD_INIT);
   TEST_CHECK(init_word_access(&w) && init_word_access(&w));
   TEST_CHECK(init_word_load(&w) == (INIT_WORD_INIT | 2));
   /* Forced: the init holder, the two access holders kept */
   TEST_CHECK(init_word_take(&w, 1, &seen) && seen == (INIT_WORD_INIT | 2));
   TEST_CHECK(init_word_load(&w) == 3 && !init_word_access(&w));
   init_word_release_access(&w);
   init_word_release_access(&w);
   TEST_CHECK(init_word_load(&w) == 1);
   init_word_cancel(&w);
   TEST_CHECK(init_word_load(&w) == 0);
   TEST_CHECK(init_word_take(&w, 0, &seen));
   init_word_cancel(&w);
   init_word_or_init(&w);
   TEST_CHECK(init_word_load(&w) == INIT_WORD_INIT);
   retro_atomic_store_release_int(&w, 0);
}

static retro_atomic_int_t finalised;

static void *finaliser(void *arg)
{
   unsigned seen, val;
   (void)arg;
   TEST_CHECK(init_word_take(&w, 1, &seen));
   val = seen - (INIT_WORD_INIT - 1);
   while (val != 1)
   {
      init_word_wait(&w, val);
      val = init_word_load(&w);
   }
   retro_atomic_store_release_int(&finalised, 1);
   init_word_cancel(&w);
   return NULL;
}

static void woken(void)
{
   pthread_t t;
   unsigned seen;
   int i;

   TEST_CHECK(init_word_take(&w, 0, &seen));
   init_word_set_init(&w);
   TEST_CHECK(init_word_access(&w));
   pthread_create(&t, NULL, finaliser, NULL);
   for (i = 0; i < 1000; i++)
      sched_yield();
   TEST_CHECK(!retro_atomic_load_acquire_int(&finalised));
   /* The last access holder leaves: the finaliser goes on */
   init_word_release_access(&w);
   pthread_join(t, NULL);
   TEST_CHECK(retro_atomic_load_acquire_int(&finalised) && init_word_load(&w) == 0);
}

#define ROUNDS 200000

static void *user(void *arg)
{
   int i;
   (void)arg;
   for (i = 0; i < ROUNDS; i++)
   {
      if (init_word_access(&w))
         init_word_release_access(&w);
   }
   return NULL;
}

static void together(void)
{
   pthread_t t[4];
   unsigned seen;
   int i;

   TEST_CHECK(init_word_take(&w, 0, &seen));
   init_word_set_init(&w);
   for (i = 0; i < 4; i++)
      pthread_create(&t[i], NULL, user, NULL);
   for (i = 0; i < 4; i++)
      pthread_join(t[i], NULL);
   TEST_CHECK(init_word_load(&w) == INIT_WORD_INIT);
}

int main(void)
{
   alarm(300);
   alone();
   woken();
   together();
   printf("test_init_word: %s\n", test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
