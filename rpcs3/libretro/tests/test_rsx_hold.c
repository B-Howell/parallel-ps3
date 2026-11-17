/* Other threads holding the renderer still (rsx_hold.c): a renderer thread
 * doing work, two threads holding it 20000 times each - never while it
 * works, with a fault served while held - then gone, and an ask after
 * returns at once. Under ThreadSanitizer too. A lost wake hangs. C89. */

#include "../../Emu/RSX/Common/rsx_hold.h"
#include "test_common.h"

#include <rthreads/rthreads.h>
#include <rthreads/retro_waitaddr.h>

#define HOLDS 20000

static rsx_hold_t hold;
static rsx_wake_t wake;
static retro_atomic_int_t working, holders, stop, served, faults, overlap;

static void serve(void *ctx)
{
   (void)ctx;
   if (retro_atomic_exchange_int(&faults, 0))
   {
      retro_atomic_inc_int(&served);
      retro_waitaddr_notify_all(&served);
   }
}

static void renderer(void *data)
{
   (void)data;
   while (!retro_atomic_load_acquire_int(&stop))
   {
      if (rsx_hold_asked(&hold))
         rsx_hold_park(&hold, &wake, NULL, serve, NULL);

      retro_atomic_store_release_int(&working, 1);
      if (retro_atomic_load_acquire_int(&holders))
         retro_atomic_inc_int(&overlap);
      retro_atomic_store_release_int(&working, 0);
   }
   rsx_hold_gone(&hold);
}

static void holder(void *data)
{
   unsigned i;
   (void)data;
   for (i = 0; i < HOLDS; i++)
   {
      rsx_hold_ask(&hold);
      retro_atomic_inc_int(&holders);
      if (retro_atomic_load_acquire_int(&working))
         retro_atomic_inc_int(&overlap);
      if (!(i & 255))
      {
         /* A fault the renderer, held, answers */
         retro_waitaddr_t on;
         on.addr = &served;
         on.old  = (uint32_t)retro_atomic_load_seq_cst_int(&served);
         retro_atomic_store_release_int(&faults, 1);
         rsx_wake_signal(&wake);
         /* Served - this one, or the other holder's with it */
         while ((uint32_t)retro_atomic_load_seq_cst_int(&served) == on.old)
            retro_waitaddr_wait(&on, 1, -1);
      }
      retro_atomic_dec_int(&holders);
      rsx_hold_release(&hold, &wake);
   }
}

int main(void)
{
   sthread_t *r, *a, *b;

   rsx_hold_init(&hold);
   rsx_wake_init(&wake);
   r = sthread_create(renderer, NULL);
   a = sthread_create(holder, NULL);
   b = sthread_create(holder, NULL);
   sthread_join(a);
   sthread_join(b);
   retro_atomic_store_release_int(&stop, 1);
   sthread_join(r);

   TEST_CHECK(retro_atomic_load_acquire_int(&overlap) == 0);
   TEST_CHECK(retro_atomic_load_acquire_int(&served) > 0);
   TEST_CHECK(!rsx_hold_asked(&hold));

   /* Gone: an ask returns at once */
   rsx_hold_ask(&hold);
   rsx_hold_release(&hold, &wake);

   if (test_failures)
   {
      fprintf(stderr, "test_rsx_hold: %d failed\n", test_failures);
      return 1;
   }
   printf("test_rsx_hold: %d holds on two threads, never while the renderer works, faults served held, none after it is gone\n", 2 * HOLDS);
   return 0;
}
