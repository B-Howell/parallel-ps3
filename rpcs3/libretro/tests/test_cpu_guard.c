/* suspend_all's guard with no lock (rpcs3/Emu/CPU/cpu_guard.c), where it
 * was a shared_mutex (s_cpu_lock):
 *
 * - one suspend at a time: suspenders on three threads never overlap;
 * - a thread entering the list (present, then cpu_guard_wait_suspend, then
 *   running) never runs in a suspend that began before it was there: each
 *   suspend takes who is present, and none it did not take is running
 *   while it holds;
 * - a thread leaving the list (taken out, then cpu_guard_retire, then
 *   freed) is never freed under a walker (cpu_guard_read_begin, _end) or
 *   a suspend that read it before: the object read is never seen freed.
 *
 * Each for a fixed number of rounds; the threads wait on the guard's
 * words, so a lost wake hangs the test, which the suite's time limit
 * catches. */

#include "../../Emu/CPU/cpu_guard.h"
#include "test_common.h"

#include <pthread.h>
#include <stdio.h>

#define ENTERERS   4
#define ROUNDS     20000
#define LIVE       0x11u
#define FREED      0xDDu

static cpu_guard_t g;
static retro_atomic_int_t present[ENTERERS];
static retro_atomic_int_t running[ENTERERS];
static retro_atomic_int_t in_suspend;
static retro_atomic_int_t overlaps;
static retro_atomic_int_t missed;
static retro_atomic_int_t stop;

/* The slot walkers read, with the object in it, and the one retired */
typedef struct obj { retro_atomic_int_t magic; } obj_t;
static obj_t objs[2];
static retro_atomic_ptr_t slot;
static retro_atomic_int_t seen_freed;

/* The object read, held a while, as a walker does */
static void touch_slot(void)
{
   obj_t *o = (obj_t*)retro_atomic_load_acquire_ptr(&slot);
   volatile int spin;
   if (!o)
      return;
   for (spin = 0; spin < 200; spin++) {}
   if ((unsigned)retro_atomic_load_acquire_int(&o->magic) != LIVE)
      retro_atomic_inc_int(&seen_freed);
}

static void *suspender(void *arg)
{
   int r, i;
   (void)arg;
   for (r = 0; r < ROUNDS; r++)
   {
      int taken[ENTERERS];

      cpu_guard_suspend_begin(&g);
      if (retro_atomic_fetch_add_int(&in_suspend, 1) != 0)
         retro_atomic_inc_int(&overlaps);

      for (i = 0; i < ENTERERS; i++)
         taken[i] = retro_atomic_load_acquire_int(&present[i]);

      touch_slot();

      /* A while with the threads held: none not taken may run */
      for (i = 0; i < 4; i++)
         touch_slot();
      for (i = 0; i < ENTERERS; i++)
         if (!taken[i] && retro_atomic_load_acquire_int(&running[i]))
            retro_atomic_inc_int(&missed);

      retro_atomic_fetch_sub_int(&in_suspend, 1);
      cpu_guard_suspend_end(&g);
   }
   return NULL;
}

static void *enterer(void *arg)
{
   const int i = (int)(size_t)arg;
   while (!retro_atomic_load_acquire_int(&stop))
   {
      retro_atomic_store_release_int(&present[i], 1);
      cpu_guard_wait_suspend(&g);
      {
         volatile int spin;
         retro_atomic_store_release_int(&running[i], 1);
         for (spin = 0; spin < 200; spin++) {}
         retro_atomic_store_release_int(&running[i], 0);
      }
      retro_atomic_store_release_int(&present[i], 0);
      cpu_guard_retire(&g);
   }
   return NULL;
}

static void *walker(void *arg)
{
   (void)arg;
   while (!retro_atomic_load_acquire_int(&stop))
   {
      obj_t *o;
      volatile int spin;

      cpu_guard_read_begin(&g);
      o = (obj_t*)retro_atomic_load_acquire_ptr(&slot);
      for (spin = 0; spin < 2000; spin++) {}
      if ((unsigned)retro_atomic_load_acquire_int(&o->magic) != LIVE)
         retro_atomic_inc_int(&seen_freed);
      cpu_guard_read_end(&g);
   }
   return NULL;
}

static void *retirer(void *arg)
{
   int r;
   (void)arg;
   for (r = 0; r < ROUNDS; r++)
   {
      obj_t *old = &objs[r & 1];
      obj_t *next = &objs[(r + 1) & 1];

      retro_atomic_store_release_int(&next->magic, LIVE);
      retro_atomic_store_release_ptr(&slot, next);
      {
         /* Walkers taking the old one meanwhile, as they would */
         volatile int spin;
         for (spin = 0; spin < 500; spin++) {}
      }
      cpu_guard_retire(&g);

      /* Freed: nothing may read it now */
      retro_atomic_store_release_int(&old->magic, FREED);
   }
   return NULL;
}

int main(void)
{
   pthread_t s[3], e[ENTERERS], w[2], rt;
   int i;

   retro_atomic_store_release_int(&objs[0].magic, LIVE);
   retro_atomic_store_release_ptr(&slot, &objs[0]);

   for (i = 0; i < ENTERERS; i++)
      pthread_create(&e[i], NULL, enterer, (void*)(size_t)i);
   for (i = 0; i < 2; i++)
      pthread_create(&w[i], NULL, walker, NULL);
   pthread_create(&rt, NULL, retirer, NULL);
   for (i = 0; i < 3; i++)
      pthread_create(&s[i], NULL, suspender, NULL);

   for (i = 0; i < 3; i++)
      pthread_join(s[i], NULL);
   pthread_join(rt, NULL);
   retro_atomic_store_release_int(&stop, 1);
   for (i = 0; i < ENTERERS; i++)
      pthread_join(e[i], NULL);
   for (i = 0; i < 2; i++)
      pthread_join(w[i], NULL);

   TEST_CHECK(retro_atomic_load_acquire_int(&overlaps) == 0);
   TEST_CHECK(retro_atomic_load_acquire_int(&missed) == 0);
   TEST_CHECK(retro_atomic_load_acquire_int(&seen_freed) == 0);
   TEST_CHECK((retro_atomic_load_acquire_int(&g.epoch) & 1) == 0);
   TEST_CHECK(retro_atomic_load_acquire_int(&g.readers) == 0);

   printf("test_cpu_guard: %d suspends on 3 threads, %d enterers, %d retires under 2 walkers: %d overlaps, %d run unheld, %d freed seen; %s\n",
         3 * ROUNDS, ENTERERS, ROUNDS, retro_atomic_load_acquire_int(&overlaps), retro_atomic_load_acquire_int(&missed),
         retro_atomic_load_acquire_int(&seen_freed), test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
