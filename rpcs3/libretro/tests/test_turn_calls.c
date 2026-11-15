/* Calls on one object taken in turn (Emu/CPU/turn_calls.c): four threads
 * make 50000 calls each on one object that only the calls touch, with no
 * atomics of their own. Every call runs, once, never two at once, and
 * each thread's calls run in the order it made them. C89. */

#include "../../Emu/CPU/turn_calls.h"
#include "test_common.h"

#include <pthread.h>

#define THREADS 4
#ifndef CALLS
#define CALLS   50000
#endif

static turn_calls_t turns;

/* Only the calls touch these */
static unsigned long total;
static int inside;
static int overlaps;
static unsigned long last[THREADS];
static int out_of_order;

struct call_ctx
{
   unsigned thread;
   unsigned long n;
};

static void step(void *p)
{
   struct call_ctx *c = (struct call_ctx *)p;

   if (inside++)
      overlaps++;

   total++;
   if (c->n != last[c->thread] + 1)
      out_of_order++;
   last[c->thread] = c->n;

   inside--;
}

static void *caller(void *arg)
{
   struct call_ctx c;
   unsigned long n;

   c.thread = (unsigned)(size_t)arg;
   for (n = 1; n <= CALLS; n++)
   {
      c.n = n;
      turn_calls_call(&turns, step, &c);
   }
   return NULL;
}

int main(void)
{
   pthread_t t[THREADS];
   size_t i;

   turn_calls_init(&turns);
   for (i = 0; i < THREADS; i++)
      pthread_create(&t[i], NULL, caller, (void *)i);
   for (i = 0; i < THREADS; i++)
      pthread_join(t[i], NULL);

   TEST_CHECK(total == (unsigned long)THREADS * CALLS);
   TEST_CHECK(overlaps == 0);
   TEST_CHECK(out_of_order == 0);
   TEST_CHECK(!turn_calls_busy(&turns));
   TEST_CHECK(mpsc_stack_empty(&turns.calls));

   if (test_failures)
   {
      printf("test_turn_calls: FAIL\n");
      return 1;
   }
   printf("test_turn_calls: ok\n");
   return 0;
}
