/* Words of guest memory changed atomically from the host (guest_atomic.h):
 * big-endian bytes whatever the host, exchange and compare-and-swap with
 * the old value as the guest sees it, adds that wrap, and two threads
 * adding to one word losing nothing. C89. */

#include "test_common.h"

#include "../../Emu/Memory/guest_atomic.h"

#include <pthread.h>

static union
{
   uint64_t align;
   unsigned char b[16];
} s_mem;

static void *adder(void *arg)
{
   unsigned i;
   (void)arg;
   for (i = 0; i < 100000; i++)
      guest_atomic_fetch_add_be32(s_mem.b + 8, 3);
   return NULL;
}

int main(void)
{
   void *w = s_mem.b;
   void *d = s_mem.b + 8;
   pthread_t t1, t2;
   static const unsigned char be32[4] = { 0x12, 0x34, 0x56, 0x78 };
   static const unsigned char be64[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };

   guest_atomic_store_be32(w, 0x12345678u);
   TEST_CHECK(memcmp(s_mem.b, be32, 4) == 0);
   TEST_CHECK(guest_atomic_load_be32(w) == 0x12345678u);
   TEST_CHECK(guest_atomic_exchange_be32(w, 0xabadcafeu) == 0x12345678u);
   TEST_CHECK(s_mem.b[0] == 0xab && s_mem.b[3] == 0xfe);
   TEST_CHECK(!guest_atomic_cas_be32(w, 0x12345678u, 1));
   TEST_CHECK(guest_atomic_load_be32(w) == 0xabadcafeu);
   TEST_CHECK(guest_atomic_cas_be32(w, 0xabadcafeu, 1));
   TEST_CHECK(s_mem.b[3] == 1 && s_mem.b[0] == 0);
   guest_atomic_store_be32(w, 0xffffffffu);
   TEST_CHECK(guest_atomic_fetch_add_be32(w, 2) == 0xffffffffu && guest_atomic_load_be32(w) == 1);

   guest_atomic_store_be64(d, 0x0123456789abcdefull);
   TEST_CHECK(memcmp(s_mem.b + 8, be64, 8) == 0);
   TEST_CHECK(guest_atomic_load_be64(d) == 0x0123456789abcdefull);
   TEST_CHECK(guest_atomic_exchange_be64(d, 7) == 0x0123456789abcdefull && s_mem.b[15] == 7);
   TEST_CHECK(!guest_atomic_cas_be64(d, 8, 9) && guest_atomic_cas_be64(d, 7, 0x100) && s_mem.b[14] == 1);

   /* A 64-bit add that carries, and one that takes away */
   guest_atomic_store_be64(d, 0xffffffffull);
   TEST_CHECK(guest_atomic_fetch_add_be64(d, 1) == 0xffffffffull && s_mem.b[11] == 1 && s_mem.b[12] == 0);
   TEST_CHECK(guest_atomic_fetch_add_be64(d, (uint64_t)0 - 2) == 0x100000000ull && guest_atomic_load_be64(d) == 0xfffffffeull);

   /* Two threads adding to one word: 600000 in all */
   guest_atomic_store_be64(d, 0);
   pthread_create(&t1, NULL, adder, NULL);
   pthread_create(&t2, NULL, adder, NULL);
   pthread_join(t1, NULL);
   pthread_join(t2, NULL);
   TEST_CHECK(guest_atomic_load_be32(d) == 600000u);
   TEST_CHECK(s_mem.b[8] == 0 && s_mem.b[11] == (600000u & 0xff));

   if (test_failures)
   {
      fprintf(stderr, "test_guest_atomic: %d failures\n", test_failures);
      return 1;
   }
   printf("test_guest_atomic: ok\n");
   return 0;
}
