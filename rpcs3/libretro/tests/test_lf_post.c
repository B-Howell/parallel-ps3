/* Changes posted from any thread, applied by one at a time (lf_post.h):
 * every change posted is applied, once, in the order each thread posted
 * it, never by two threads at once, and no poster waits. Run under
 * ThreadSanitizer: the applier's state is plain memory. C89. */

#include <stdlib.h>

#include "../../../Utilities/lf_post.h"
#include "test_common.h"

#include <rthreads/rthreads.h>
#include <sched.h>

#define THREADS 4
#ifndef POSTS
#define POSTS 50000
#endif

typedef struct change
{
   mpsc_stack_node_t node;
   unsigned thread;
   unsigned seq;
} change_t;

static lf_post_t post;
static retro_atomic_int_t inside;
static retro_atomic_int_t overlaps;

/* The applier's alone: plain memory */
static unsigned long applied;
static unsigned last_seq[THREADS];
static unsigned out_of_order;

static void apply(void *ctx, mpsc_stack_node_t *oldest)
{
   (void)ctx;
   if (retro_atomic_fetch_add_int(&inside, 1) != 0)
      retro_atomic_inc_int(&overlaps);

   while (oldest)
   {
      mpsc_stack_node_t *next = oldest->next;
      change_t *c = (change_t*)oldest;
      if (c->seq != last_seq[c->thread] + 1u)
         out_of_order++;
      last_seq[c->thread] = c->seq;
      applied++;
      free(c);
      oldest = next;
   }

   retro_atomic_dec_int(&inside);
}

static void poster(void *data)
{
   const unsigned id = (unsigned)(size_t)data;
   unsigned i;

   for (i = 1; i <= POSTS; i++)
   {
      change_t *c = (change_t*)malloc(sizeof(*c));
      c->thread = id;
      c->seq    = i;
      lf_post(&post, &c->node, apply, NULL);
   }
}

/* A change posted while another thread applies: the poster goes on at
 * once, and the applier, done with what it took, applies it too */
static lf_post_t post2;
static retro_atomic_int_t entered;
static retro_atomic_int_t go_on;
static unsigned applied2;

static void apply2(void *ctx, mpsc_stack_node_t *oldest)
{
   (void)ctx;
   if (!retro_atomic_exchange_int(&entered, 1))
      while (!retro_atomic_load_acquire_int(&go_on))
         sched_yield();
   while (oldest)
   {
      mpsc_stack_node_t *next = oldest->next;
      free(oldest);
      applied2++;
      oldest = next;
   }
}

static void first_poster(void *data)
{
   (void)data;
   lf_post(&post2, (mpsc_stack_node_t*)calloc(1, sizeof(change_t)), apply2, NULL);
}

static void while_applying(void)
{
   sthread_t *t;

   lf_post_init(&post2);
   retro_atomic_int_init(&entered, 0);
   retro_atomic_int_init(&go_on, 0);
   t = sthread_create(first_poster, NULL);
   while (!retro_atomic_load_acquire_int(&entered))
      sched_yield();

   /* Returns at once: the other thread is applying */
   lf_post(&post2, (mpsc_stack_node_t*)calloc(1, sizeof(change_t)), apply2, NULL);
   TEST_CHECK(!mpsc_stack_empty(&post2.posted));

   retro_atomic_store_release_int(&go_on, 1);
   sthread_join(t);
   TEST_CHECK(mpsc_stack_empty(&post2.posted));
   TEST_CHECK(applied2 == 2);
}

int main(void)
{
   sthread_t *t[THREADS];
   unsigned i;

   while_applying();
   lf_post_init(&post);
   retro_atomic_int_init(&inside, 0);
   retro_atomic_int_init(&overlaps, 0);

   for (i = 0; i < THREADS; i++)
      t[i] = sthread_create(poster, (void*)(size_t)i);
   for (i = 0; i < THREADS; i++)
      sthread_join(t[i]);

   /* Nothing left behind: the last poster out applied all of it */
   TEST_CHECK(mpsc_stack_empty(&post.posted));
   TEST_CHECK(applied == (unsigned long)THREADS * POSTS);
   TEST_CHECK(!out_of_order);
   TEST_CHECK(retro_atomic_load_acquire_int(&overlaps) == 0);

   if (test_failures)
   {
      fprintf(stderr, "test_lf_post: %d failed\n", test_failures);
      return 1;
   }
   printf("test_lf_post: %lu changes from %d threads, each applied once, in order, by one thread at a time\n", applied, THREADS);
   return 0;
}
