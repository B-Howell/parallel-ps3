/* Changes to an object that one thread at a time applies, posted from any
 * thread, with no lock and no thread waiting for another. C89.
 *
 * A thread with a change for the object pushes it, as a node, onto the
 * object's stack and then tries to become its applier with one
 * compare-and-swap. The applier takes everything posted, applies it
 * oldest first, and lets go; if more was posted meanwhile, it tries again,
 * so nothing posted is left behind. A thread that finds another applying
 * goes on at once: its change is applied by that one. Nobody waits for a
 * change to be applied, so it suits changes whose poster wants no answer:
 * a cache filled from the network, a list a reader takes a snapshot of.
 * What the applier changes is its alone; what other threads read it
 * publishes (an atomic pointer, retro_retire.h). */

#ifndef LF_POST_H
#define LF_POST_H

#include <retro_atomic.h>
#include <retro_inline.h>
#include <queues/mpsc_stack.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lf_post
{
   mpsc_stack_t       posted;
   retro_atomic_int_t busy;
} lf_post_t;

/* Applies the changes in @oldest, a list in the order posted. The nodes
 * are the callee's to free */
typedef void (*lf_post_apply_t)(void *ctx, mpsc_stack_node_t *oldest);

/* For one not zero-filled already */
static INLINE void lf_post_init(lf_post_t *p)
{
   mpsc_stack_init(&p->posted);
   retro_atomic_int_init(&p->busy, 0);
}

/* Applies what is posted, if no other thread is: any thread */
static INLINE void lf_post_apply(lf_post_t *p, lf_post_apply_t fn, void *ctx)
{
   while (!mpsc_stack_empty(&p->posted) && retro_atomic_cas_int(&p->busy, 0, 1))
   {
      mpsc_stack_node_t *newest = mpsc_stack_drain(&p->posted);
      mpsc_stack_node_t *oldest = NULL;

      while (newest)
      {
         mpsc_stack_node_t *next = newest->next;
         newest->next = oldest;
         oldest       = newest;
         newest       = next;
      }

      if (oldest)
         fn(ctx, oldest);
#ifdef LF_POST_TEST_WINDOW
      /* The tests widen the window a poster may come in */
      LF_POST_TEST_WINDOW();
#endif

      /* Let go, then look again: a change posted after the drain, by a
       * thread that found this one applying, is not left behind. The
       * fence pairs with the poster's: either this sees its change, or it
       * sees the applier gone and applies it itself */
      (void)retro_atomic_exchange_int(&p->busy, 0);
      retro_atomic_thread_fence_seq_cst();
   }
}

/* Posts @change and applies what is posted, if no other thread is */
static INLINE void lf_post(lf_post_t *p, mpsc_stack_node_t *change,
      lf_post_apply_t fn, void *ctx)
{
   mpsc_stack_push(&p->posted, change);
   retro_atomic_thread_fence_seq_cst();
   lf_post_apply(p, fn, ctx);
}

#ifdef __cplusplus
}
#endif

#endif
