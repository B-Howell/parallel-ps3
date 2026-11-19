/* The SPU instruction slot counts (Emu/Cell/spu_pc_table.c): four slots
 * to a 32-bit word, a waiter flag in each.
 *
 * - Alone: counts per slot, the slots of one word apart; a wait for a
 *   count below its limit returns at once; one that nobody ends runs its
 *   bound out and says so.
 * - A waiter on a full slot is woken by the leave, long before its 2 s
 *   bound (the wait says whether the bound ran out).
 * - Four threads entering and leaving the four slots of one word at
 *   once: none of the counts lost, none spilling into a neighbour. */

#include "../../Emu/Cell/spu_pc_table.h"
#include "test_common.h"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <retro_atomic.h>

static spu_pc_table_t table;

static void alone(void)
{
   spu_pc_enter(&table, 8);
   spu_pc_enter(&table, 8);
   spu_pc_enter(&table, 9);
   TEST_CHECK(spu_pc_count(&table, 8) == 2);
   TEST_CHECK(spu_pc_count(&table, 9) == 1);
   TEST_CHECK(spu_pc_count(&table, 10) == 0);
   TEST_CHECK(spu_pc_count(&table, 11) == 0);

   TEST_CHECK(spu_pc_wait_below(&table, 8, 3, 1000) == 1);
   /* Full and nobody leaving: the bound runs out */
   TEST_CHECK(spu_pc_wait_below(&table, 8, 2, 1000) == 0);

   spu_pc_leave(&table, 8);
   spu_pc_leave(&table, 8);
   spu_pc_leave(&table, 9);
   TEST_CHECK(spu_pc_count(&table, 8) == 0 && spu_pc_count(&table, 9) == 0);
   /* The waiter's flag gone with the leave */
   TEST_CHECK(retro_atomic_load_acquire_int(&table.words[2]) == 0);
}

static retro_atomic_int_t waiting, woken;

static void *waiter(void *arg)
{
   (void)arg;
   retro_atomic_store_release_int(&waiting, 1);
   retro_atomic_store_release_int(&woken, spu_pc_wait_below(&table, 100, 1, 2000000) + 1);
   return NULL;
}

static void woken_by_leave(void)
{
   pthread_t t;
   int i;

   spu_pc_enter(&table, 100);
   pthread_create(&t, NULL, waiter, NULL);
   while (!retro_atomic_load_acquire_int(&waiting))
      sched_yield();
   /* Let it sleep (if the leave came first it returns at once anyway) */
   for (i = 0; i < 1000; i++)
      sched_yield();
   spu_pc_leave(&table, 100);
   pthread_join(t, NULL);

   /* 2: returned 1 (below), not 0 (the bound ran out) */
   TEST_CHECK(retro_atomic_load_acquire_int(&woken) == 2);
   TEST_CHECK(spu_pc_count(&table, 100) == 0);
}

#define THREADS 4
#define ROUNDS  200000

static retro_atomic_int_t go;

static void *stepper(void *arg)
{
   const uint32_t slot = 200 + (uint32_t)(size_t)arg;
   int i;

   while (!retro_atomic_load_acquire_int(&go))
      sched_yield();

   for (i = 0; i < ROUNDS; i++)
   {
      spu_pc_enter(&table, slot);
      spu_pc_enter(&table, slot);
      spu_pc_leave(&table, slot);
      spu_pc_leave(&table, slot);
   }

   return NULL;
}

static void together(void)
{
   pthread_t t[THREADS];
   size_t i;

   /* A neighbour standing at 5 the whole time */
   for (i = 0; i < 5; i++)
      spu_pc_enter(&table, 203);

   for (i = 0; i < THREADS - 1; i++)
      pthread_create(&t[i], NULL, stepper, (void*)i);
   retro_atomic_store_release_int(&go, 1);
   for (i = 0; i < THREADS - 1; i++)
      pthread_join(t[i], NULL);

   TEST_CHECK(spu_pc_count(&table, 200) == 0);
   TEST_CHECK(spu_pc_count(&table, 201) == 0);
   TEST_CHECK(spu_pc_count(&table, 202) == 0);
   TEST_CHECK(spu_pc_count(&table, 203) == 5);
}

int main(void)
{
   alarm(300);
   alone();
   woken_by_leave();
   together();
   printf("test_spu_pc_table: %s\n", test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
