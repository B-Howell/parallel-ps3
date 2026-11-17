/* The RPCN client's sync reply slots (rpcn_reply_slots.c): a reply taken
 * only by its id, once; a full table; what is dropped as the connection
 * goes; and under ThreadSanitizer a reader publishing 100000 replies while
 * four threads each wait for their own ids - every reply reaches the one
 * thread waiting for it, with what was put with it. C89. */

#include "../../Emu/NP/rpcn_reply_slots.h"
#include "test_common.h"

#include <rthreads/rthreads.h>

#define WAITERS 4
#define PER_WAITER 5000

static rpcn_reply_slots_t slots;
static uint64_t payload[RPCN_REPLY_SLOTS]; /* what goes with a slot */
static retro_atomic_int_t bad, done;

static void waiter(void *data)
{
   const uint64_t base = (uint64_t)(uintptr_t)data;
   uint64_t n;
   for (n = 0; n < PER_WAITER; n++)
   {
      const uint64_t id = (base << 32) | n;
      int s;
      while ((s = rpcn_reply_slots_take(&slots, id)) < 0)
         retro_atomic_load_acquire_int(&done);
      if (payload[s] != (id ^ 0x5a5a5a5a))
         retro_atomic_inc_int(&bad);
      rpcn_reply_slots_free(&slots, s);
   }
}

int main(void)
{
   sthread_t *t[WAITERS];
   uint64_t n;
   int s, i, count;

   /* Taken by its id only, once */
   rpcn_reply_slots_init(&slots);
   s = rpcn_reply_slots_claim(&slots);
   TEST_CHECK(s >= 0);
   TEST_CHECK(rpcn_reply_slots_take(&slots, 7) < 0); /* being filled */
   rpcn_reply_slots_publish(&slots, s, 7);
   TEST_CHECK(rpcn_reply_slots_take(&slots, 8) < 0);
   TEST_CHECK(rpcn_reply_slots_take(&slots, 7) == s);
   TEST_CHECK(rpcn_reply_slots_take(&slots, 7) < 0);
   rpcn_reply_slots_free(&slots, s);

   /* Full, then dropped */
   for (i = 0; i < RPCN_REPLY_SLOTS; i++)
   {
      s = rpcn_reply_slots_claim(&slots);
      TEST_CHECK(s == i);
      rpcn_reply_slots_publish(&slots, s, (uint64_t)i);
   }
   TEST_CHECK(rpcn_reply_slots_claim(&slots) < 0);
   count = 0;
   for (s = rpcn_reply_slots_drop(&slots, 0); s >= 0; s = rpcn_reply_slots_drop(&slots, s + 1))
   {
      rpcn_reply_slots_free(&slots, s);
      count++;
   }
   TEST_CHECK(count == RPCN_REPLY_SLOTS);
   TEST_CHECK(rpcn_reply_slots_claim(&slots) == 0);
   rpcn_reply_slots_init(&slots);

   /* One reader, four waiters */
   for (i = 0; i < WAITERS; i++)
      t[i] = sthread_create(waiter, (void*)(uintptr_t)(i + 1));
   for (n = 0; n < PER_WAITER; n++)
   {
      for (i = 0; i < WAITERS; i++)
      {
         const uint64_t id = ((uint64_t)(i + 1) << 32) | n;
         while ((s = rpcn_reply_slots_claim(&slots)) < 0)
            retro_atomic_load_acquire_int(&done);
         payload[s] = id ^ 0x5a5a5a5a;
         rpcn_reply_slots_publish(&slots, s, id);
      }
   }
   for (i = 0; i < WAITERS; i++)
      sthread_join(t[i]);
   TEST_CHECK(retro_atomic_load_acquire_int(&bad) == 0);

   if (test_failures)
   {
      fprintf(stderr, "test_rpcn_reply_slots: %d failed\n", test_failures);
      return 1;
   }
   printf("test_rpcn_reply_slots: taken by id once, full and dropped, %d replies to %d waiters\n", WAITERS * PER_WAITER, WAITERS);
   return 0;
}
