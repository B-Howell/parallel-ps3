/* Calls run on one owner thread (Emu/CPU/owner_calls.c).
 *
 * - Four threads make 20000 calls each: every one runs on the owner, one
 *   at a time (a plain counter they add to comes out right), and the
 *   caller sees what its call wrote.
 * - Closed while callers are still calling: every call still runs once -
 *   on the owner before the close, on the caller's thread after - and the
 *   close ends once no caller is left in one.
 * - An owner that waits on a word of its own, not the wake word (as one
 *   in poll() waits on a socket): the waker is called with every wake, so
 *   2000 calls from two threads all get run. */

#include "../../Emu/CPU/owner_calls.h"
#include "test_common.h"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <stdio.h>
#include <rthreads/retro_waitaddr.h>

#define CALLERS 4
#define CALLS   20000

static unsigned calls_each = CALLS;

static owner_calls_t calls;
static pthread_t owner;
static __thread int is_owner; /* set by the owner thread on itself */
static retro_atomic_int_t stop_owner;
static retro_atomic_int_t elsewhere;
static retro_atomic_int_t ran;
static unsigned plain; /* not atomic: only one thread at a time may add */

struct call_ctx
{
   unsigned in;
   unsigned out;
   int on_owner;
};

static void the_call(void *p)
{
   struct call_ctx *c = (struct call_ctx *)p;
   int spin;

   c->on_owner = is_owner;
   if (!c->on_owner)
   {
      /* After the close, on the caller's thread: the plain counter is the
       * owner's, so it is not touched here */
      if (!(retro_atomic_load_acquire_int(&calls.callers) & OWNER_CALLS_CLOSED))
         retro_atomic_fetch_add_int(&elsewhere, 1);
   }
   else
   {
      unsigned was = plain;
      for (spin = 0; spin < 8; spin++)
         (void)retro_atomic_load_relaxed_int(&stop_owner);
      plain = was + 1;
   }
   c->out = c->in * 3 + 1;
   retro_atomic_fetch_add_int(&ran, 1);
}

static void *owner_main(void *arg)
{
   (void)arg;
   is_owner = 1;
   for (;;)
   {
      retro_waitaddr_t w;
      const int wake = retro_atomic_load_acquire_int(owner_calls_wake_word(&calls));

      owner_calls_run(&calls);

      if (retro_atomic_load_acquire_int(&stop_owner))
         break;

      w.addr = owner_calls_wake_word(&calls);
      w.old  = (uint32_t)wake;
      retro_waitaddr_wait(&w, 1, -1);
   }

   owner_calls_close(&calls);
   return NULL;
}

static retro_atomic_int_t wrong;
static retro_atomic_int_t calls_made;

static void *caller_main(void *arg)
{
   unsigned t = (unsigned)(size_t)arg;
   unsigned n;

   for (n = 0; n < calls_each; n++)
   {
      struct call_ctx c;
      c.in = t * 100000u + n;
      c.out = 0;
      c.on_owner = 0;
      owner_calls_call(&calls, the_call, &c);
      if (c.out != c.in * 3 + 1)
         retro_atomic_fetch_add_int(&wrong, 1);
      retro_atomic_fetch_add_int(&calls_made, 1);
   }
   return NULL;
}

static void round_of(int close_midway)
{
   pthread_t callers[CALLERS];
   unsigned t;

   owner_calls_init(&calls);
   retro_atomic_int_init(&stop_owner, 0);
   retro_atomic_int_init(&elsewhere, 0);
   retro_atomic_int_init(&ran, 0);
   retro_atomic_int_init(&wrong, 0);
   retro_atomic_int_init(&calls_made, 0);
   plain = 0;

   TEST_CHECK(pthread_create(&owner, NULL, owner_main, NULL) == 0);
   for (t = 0; t < CALLERS; t++)
      TEST_CHECK(pthread_create(&callers[t], NULL, caller_main, (void *)(size_t)t) == 0);

   if (close_midway)
   {
      while (retro_atomic_load_acquire_int(&calls_made) < (int)(CALLERS * calls_each / 4))
         sched_yield();
      retro_atomic_store_release_int(&stop_owner, 1);
      owner_calls_wake(&calls);
      pthread_join(owner, NULL);
   }

   for (t = 0; t < CALLERS; t++)
      pthread_join(callers[t], NULL);

   if (!close_midway)
   {
      TEST_CHECK(plain == CALLERS * calls_each);
      retro_atomic_store_release_int(&stop_owner, 1);
      owner_calls_wake(&calls);
      pthread_join(owner, NULL);
   }

   TEST_CHECK(retro_atomic_load_acquire_int(&ran) == (int)(CALLERS * calls_each));
   TEST_CHECK(retro_atomic_load_acquire_int(&wrong) == 0);
   TEST_CHECK(retro_atomic_load_acquire_int(&elsewhere) == 0);
   if (close_midway > 1)
      return;
   printf("test_owner_calls: %s: %d calls from %d threads ran, %d on another thread while open, %d wrong results; counted %u\n",
         close_midway ? "closed midway" : "open", retro_atomic_load_acquire_int(&ran), CALLERS,
         retro_atomic_load_acquire_int(&elsewhere), retro_atomic_load_acquire_int(&wrong), plain);
}

/* The waker's owner: waits on its own word, which only the waker bumps */
static owner_calls_t wcalls;
static retro_atomic_int_t own_word, wakes, wstop, wran;

static void waker(void *ctx)
{
   (void)ctx;
   retro_atomic_fetch_add_int(&wakes, 1);
   retro_atomic_fetch_add_int(&own_word, 1);
   retro_waitaddr_notify_all(&own_word);
}

static void *waker_owner(void *p)
{
   (void)p;
   while (!retro_atomic_load_acquire_int(&wstop))
   {
      retro_waitaddr_t on;
      on.addr = &own_word;
      on.old  = (uint32_t)retro_atomic_load_acquire_int(&own_word);
      owner_calls_run(&wcalls);
      if (retro_atomic_load_acquire_int(&wstop))
         break;
      retro_waitaddr_wait(&on, 1, -1);
   }
   owner_calls_close(&wcalls);
   return NULL;
}

static void wcall(void *p)
{
   (void)p;
   retro_atomic_fetch_add_int(&wran, 1);
}

static void *waker_caller(void *p)
{
   int i;
   (void)p;
   for (i = 0; i < 1000; i++)
      owner_calls_call(&wcalls, wcall, NULL);
   return NULL;
}

static void waker_round(void)
{
   pthread_t o, c[2];
   int i;

   owner_calls_init(&wcalls);
   owner_calls_set_waker(&wcalls, waker, NULL);
   retro_atomic_int_init(&own_word, 0);
   retro_atomic_int_init(&wakes, 0);
   retro_atomic_int_init(&wstop, 0);
   retro_atomic_int_init(&wran, 0);

   pthread_create(&o, NULL, waker_owner, NULL);
   for (i = 0; i < 2; i++)
      pthread_create(&c[i], NULL, waker_caller, NULL);
   for (i = 0; i < 2; i++)
      pthread_join(c[i], NULL);

   retro_atomic_store_release_int(&wstop, 1);
   waker(NULL);
   pthread_join(o, NULL);

   TEST_CHECK(retro_atomic_load_acquire_int(&wran) == 2000);
   TEST_CHECK(retro_atomic_load_acquire_int(&wakes) > 0);
   printf("test_owner_calls: an owner waiting on its own word: %d calls run, woken by the waker\n", retro_atomic_load_acquire_int(&wran));
}

int main(void)
{
   int r;
   alarm(300);
   round_of(0);
   round_of(1);
   /* Many short rounds closed midway: a caller between counting itself
    * and handing its call over when the close comes must still be run */
   calls_each = 200;
   for (r = 0; r < 2000; r++)
      round_of(2);
   printf("test_owner_calls: 2000 short rounds closed midway, every call run\n");
   waker_round();
   printf("test_owner_calls: %s\n", test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
