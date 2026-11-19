/* The reader/writer word behind shared_mutex (Utilities/rw_word.c).
 *
 * - Alone: readers counted, a writer refused while they hold it and they
 *   refused while it does, a lone reader upgraded and downgraded, a
 *   broken word reported by the next lock.
 * - Two writers and two readers at once (100000 rounds each): a writer
 *   steps a counter twice under the lock and a reader, under its own,
 *   must never see it odd; the count ends right and the word at 0, so
 *   every sleeper was handed the lock and woken. */

#include "../../../Utilities/rw_word.h"
#include "test_common.h"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <retro_atomic.h>

static retro_atomic_int_t w;

static void alone(void)
{
   TEST_CHECK(rw_try_lock_shared(&w) && rw_try_lock_shared(&w));
   TEST_CHECK(rw_value(&w) == 2);
   TEST_CHECK(!rw_try_lock(&w));
   /* Two readers: no upgrade */
   TEST_CHECK(!rw_try_lock_upgrade(&w));
   TEST_CHECK(rw_unlock_shared(&w));
   /* One: upgraded to the writer, then back */
   TEST_CHECK(rw_try_lock_upgrade(&w));
   TEST_CHECK(rw_value(&w) == RW_ONE);
   TEST_CHECK(!rw_try_lock_shared(&w));
   rw_lock_downgrade(&w);
   TEST_CHECK(rw_value(&w) == 1);
   TEST_CHECK(rw_unlock_shared(&w));
   TEST_CHECK(rw_try_lock(&w));
   TEST_CHECK(!rw_try_lock_shared(&w) && !rw_try_lock(&w));
   TEST_CHECK(rw_unlock(&w));
   TEST_CHECK(rw_value(&w) == 0);
   rw_lock_unlock(&w);
   /* Unlocking what nobody holds breaks the word (as before, the unlock
    * itself does not look), and the next lock reports it */
   rw_unlock_shared(&w);
   TEST_CHECK(!rw_lock_shared(&w));
   retro_atomic_store_release_int(&w, 0);
}

#define ROUNDS 100000

static volatile unsigned counter;
static retro_atomic_int_t go, odd_seen;

static void *writer(void *arg)
{
   int i;
   (void)arg;
   while (!retro_atomic_load_acquire_int(&go))
      sched_yield();
   for (i = 0; i < ROUNDS; i++)
   {
      rw_lock(&w);
      counter = counter + 1;
      sched_yield();
      counter = counter + 1;
      rw_unlock(&w);
   }
   return NULL;
}

static void *reader(void *arg)
{
   int i;
   (void)arg;
   while (!retro_atomic_load_acquire_int(&go))
      sched_yield();
   for (i = 0; i < ROUNDS; i++)
   {
      rw_lock_shared(&w);
      if (counter & 1)
         retro_atomic_fetch_add_int(&odd_seen, 1);
      rw_unlock_shared(&w);
   }
   return NULL;
}

static void together(void)
{
   pthread_t t[4];
   int i;

   pthread_create(&t[0], NULL, writer, NULL);
   pthread_create(&t[1], NULL, writer, NULL);
   pthread_create(&t[2], NULL, reader, NULL);
   pthread_create(&t[3], NULL, reader, NULL);
   retro_atomic_store_release_int(&go, 1);
   for (i = 0; i < 4; i++)
      pthread_join(t[i], NULL);

   TEST_CHECK(retro_atomic_load_acquire_int(&odd_seen) == 0);
   TEST_CHECK(counter == 4u * ROUNDS);
   TEST_CHECK(rw_value(&w) == 0);
}

int main(void)
{
   alarm(300);
   alone();
   together();
   printf("test_rw_word: %s\n", test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
