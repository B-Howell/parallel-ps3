/* The memory range locks (Emu/Memory/vm_range_locks.c): slots taken and
 * given back with compare-and-swap, and the scans the readers, the memory
 * setting changes and the writers make over them.
 *
 * - Alone: slots come lowest first; one below the top bit is always left
 *   free, and the top bit is never handed out, so after 62 the next take
 *   gives -1; a slot given back is cleared and is
 *   the next one taken.
 * - The three scans give the same slots as the C++ code they replace
 *   (written out again below as it was), over hand-picked cases (a slot
 *   locked whole covering 128 bytes, a shared memory mirror, a range
 *   wrapping the 32-bit space, a range starting on a 64K page) and over
 *   200000 random sets of slots.
 * - Four threads taking and giving back at once: no slot is ever held by
 *   two of them, and at the end none is taken.
 * - A reader setting its slot and a writer setting its bit, at once, over
 *   200000 rounds: at least one of the two always sees the other (the
 *   full barrier after each side's write). */

#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "../../Emu/Memory/vm_range_locks.h"
#include "test_common.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include <retro_atomic.h>
#include <rthreads/retro_waitaddr.h>

static retro_atomic_64_t bits;
static vm_range_lock_slot_t set[VM_RANGE_LOCKS];
static retro_atomic_64_t shmem[65536];

static uint64_t word_of(unsigned id)
{
   return (uint64_t)retro_atomic_load_acquire_64(&set[id].word);
}

static void put(unsigned id, uint64_t v)
{
   retro_atomic_store_release_64(&set[id].word, (int64_t)v);
}

static uint64_t shmem_at(uint64_t page)
{
   return (uint64_t)retro_atomic_load_acquire_64(&shmem[page & 0xffff]);
}

/* ---- the C++ code as it was (vm.cpp: for_all_range_locks and the three
 * lambdas), so a scan can be checked against it */

static int old_reader_hit(uint64_t begin, uint32_t size, uint64_t is_share,
      uint64_t addr_exec, uint32_t size_exec)
{
   uint64_t addr = begin;
   uint64_t is_shared;

   if ((size_exec & (7u << 29)) == (4u << 29))
   {
      size_exec = 128;
      if (is_share)
         addr = (uint16_t)addr | is_share;
   }

   size_exec = (size_exec << 3) >> 3;

   if ((is_shared = shmem_at(addr_exec >> 16)))
      addr_exec = (uint16_t)addr_exec | is_shared;

   return addr <= addr_exec + size_exec - 1 && addr_exec <= addr + size - 1;
}

static int old_overlap_hit(uint32_t addr, uint32_t size, uint32_t addr2, uint32_t size2)
{
   const uint32_t end  = addr + size - 1;
   const uint32_t end2 = addr2 + size2 - 1;
   return addr <= end2 && addr2 <= end;
}

static int old_point_hit(uint64_t point, uint64_t addr2, uint32_t size2)
{
   uint64_t hi, max;

   if ((size2 >> 29) == 2)
      return 0;

   for (hi = addr2 >> 16, max = (addr2 + size2 - 1) >> 16; hi <= max; hi++)
   {
      uint64_t addr3 = addr2;
      uint64_t a     = addr2 + size2;
      uint64_t b     = (addr2 + 0xffff) & ~(uint64_t)0xffff;
      uint64_t size3 = (a < b ? a : b) - addr2;
      uint64_t is_shared;

      if ((is_shared = shmem_at(hi)))
         addr3 = (uint16_t)addr2 | is_shared;

      if (point - (addr3 / 128) <= (addr3 + size3 - 1) / 128 - (addr3 / 128))
         return 1;

      addr2 += size3;
      size2 -= (uint32_t)size3;
   }

   return 0;
}

/* kind: 0 reader, 1 overlap, 2 point */
static uint64_t old_scan(int kind, uint64_t mask, uint32_t begin, uint32_t size,
      uint64_t is_share, uint64_t point)
{
   uint64_t result = mask;
   uint64_t b;

   for (b = mask; b; b &= b - 1)
   {
      const unsigned id    = (unsigned)__builtin_ctzll(b);
      const uint64_t v     = word_of(id);
      const uint32_t size2 = (uint32_t)(v >> 32);
      int hit              = 0;

      if (size2)
      {
         const uint32_t addr2 = (uint32_t)v;
         if (kind == 0)
            hit = old_reader_hit(begin, size, is_share, addr2, size2);
         else if (kind == 1)
            hit = old_overlap_hit(begin, size, addr2, size2);
         else
            hit = old_point_hit(point, addr2, size2);
      }

      if (!hit)
         result &= ~((uint64_t)1 << id);
   }

   return result;
}

static uint64_t new_scan(int kind, uint64_t mask, uint32_t begin, uint32_t size,
      uint64_t is_share, uint64_t point)
{
   if (kind == 0)
      return vm_range_locks_busy_reader(mask, set, shmem, begin, size, is_share);
   if (kind == 1)
      return vm_range_locks_busy_overlap(mask, set, begin, size);
   return vm_range_locks_busy_point(mask, set, shmem, point);
}

static void clear_all(void)
{
   unsigned i;
   for (i = 0; i < VM_RANGE_LOCKS; i++)
      put(i, 0);
   for (i = 0; i < 65536; i++)
      retro_atomic_store_relaxed_64(&shmem[i], 0);
   retro_atomic_store_release_64(&bits, 0);
}

static void alone(void)
{
   int i;

   clear_all();
   for (i = 0; i < 62; i++)
      TEST_CHECK(vm_range_lock_alloc(&bits) == i);
   /* The top bit is kept for memory setting changes, and the one below it
    * never handed out either (as the C++ code had it) */
   TEST_CHECK(vm_range_lock_alloc(&bits) == -1);
   TEST_CHECK((uint64_t)retro_atomic_load_acquire_64(&bits) == ~(uint64_t)0 >> 2);

   put(5, 0x1234);
   vm_range_lock_free(&bits, set, 5);
   TEST_CHECK(word_of(5) == 0);
   vm_range_lock_free(&bits, set, 40);
   TEST_CHECK(vm_range_lock_alloc(&bits) == 5);
   TEST_CHECK(vm_range_lock_alloc(&bits) == 40);
   TEST_CHECK(vm_range_lock_alloc(&bits) == -1);

   for (i = 0; i < 62; i++)
      vm_range_lock_free(&bits, set, (unsigned)i);
   TEST_CHECK(retro_atomic_load_acquire_64(&bits) == 0);
}

#define LOCKED   ((uint64_t)VM_RANGE_LOCKED_HI << 32)
#define READABLE ((uint64_t)VM_RANGE_READABLE_HI << 32)

static void picked(void)
{
   const uint64_t all = ~(uint64_t)0;

   clear_all();

   /* A plain range: [0x10000, 0x10100) */
   put(0, 0x10000 | (uint64_t)0x100 << 32);
   TEST_CHECK(new_scan(0, all, 0x100ff, 1, 0, 0) == 1);
   TEST_CHECK(new_scan(0, all, 0x10100, 4, 0, 0) == 0);
   TEST_CHECK(new_scan(0, all, 0xfff0, 0x10, 0, 0) == 0);
   TEST_CHECK(new_scan(0, all, 0xfff0, 0x11, 0, 0) == 1);
   /* Out of the mask: not looked at */
   TEST_CHECK(new_scan(0, all & ~(uint64_t)1, 0x10000, 4, 0, 0) == 0);

   /* Locked whole: 128 bytes from its address, whatever its size says */
   put(1, 0x20000 | LOCKED | (uint64_t)0x1000 << 32);
   TEST_CHECK(new_scan(0, 2, 0x2007f, 1, 0, 0) == 2);
   TEST_CHECK(new_scan(0, 2, 0x20080, 1, 0, 0) == 0);

   /* A shared memory mirror under the locked slot's page: the reader's
    * address is taken through its own mirror base */
   retro_atomic_store_release_64(&shmem[2], (int64_t)0x7000000000ull);
   TEST_CHECK(new_scan(0, 2, 0x20000, 4, 0, 0) == 0);
   TEST_CHECK(new_scan(0, 2, 0x50000, 4, 0x7000000000ull, 0) == 2);
   TEST_CHECK(new_scan(0, 2, 0x50000, 4, 0x6000000000ull, 0) == 0);

   /* Overlap: as 32-bit ranges, wrapping at the top */
   put(2, 0xffffff00u | (uint64_t)0x100 << 32);
   TEST_CHECK(new_scan(1, 4, 0xffffffff, 1, 0, 0) == 4);
   TEST_CHECK(new_scan(1, 4, 0xfffffe00, 0x100, 0, 0) == 0);

   /* Point: a readable-only slot never holds the line */
   put(3, 0x30080 | READABLE | (uint64_t)0x80 << 32);
   TEST_CHECK(new_scan(2, 8, 0, 0, 0, 0x30080 / 128) == 0);
   /* A reader's plain range: holds its own lines only */
   put(3, 0x30080 | (uint64_t)0x80 << 32);
   TEST_CHECK(new_scan(2, 8, 0, 0, 0, 0x30080 / 128) == 8);
   TEST_CHECK(new_scan(2, 8, 0, 0, 0, 0x30100 / 128) == 0);
   /* Starting on a 64K page: taken as a hit, as before */
   put(3, 0x40000 | (uint64_t)0x80 << 32);
   TEST_CHECK(new_scan(2, 8, 0, 0, 0, 0x90000 / 128) == 8);

   /* Every one of these as the old code gave */
   TEST_CHECK(new_scan(0, all, 0x20000, 0x10000, 0, 0) == old_scan(0, all, 0x20000, 0x10000, 0, 0));
   TEST_CHECK(new_scan(1, all, 0x10010, 0x20000, 0, 0) == old_scan(1, all, 0x10010, 0x20000, 0, 0));
   TEST_CHECK(new_scan(2, all, 0, 0, 0, 0x40000 / 128) == old_scan(2, all, 0, 0, 0, 0x40000 / 128));
}

static uint64_t rnd_state = 88172645463325252ull;

static uint64_t rnd(void)
{
   rnd_state ^= rnd_state << 13;
   rnd_state ^= rnd_state >> 7;
   rnd_state ^= rnd_state << 17;
   return rnd_state;
}

/* Addresses near each other (so ranges meet), sizes small or not, flags
 * any; some pages mirrored */
static uint32_t near_addr(void)
{
   return (uint32_t)(0x10000 * (rnd() % 4) + (rnd() % 0x20000)) - 0x100;
}

static void random_sets(void)
{
   unsigned round, i, kind, differ = 0;

   clear_all();
   for (round = 0; round < 200000; round++)
   {
      const uint64_t mask = rnd();
      uint32_t begin, size;
      uint64_t is_share, point;

      if (round % 64 == 0)
      {
         for (i = 0; i < 8; i++)
            retro_atomic_store_relaxed_64(&shmem[i], rnd() % 3 ? 0 : (int64_t)((rnd() % 16) << 36));
      }

      for (i = 0; i < VM_RANGE_LOCKS; i++)
      {
         const uint64_t flags = (rnd() % 8) << 61;
         const uint64_t sz    = rnd() % 4 == 0 ? 0 : (rnd() % 2 ? rnd() % 0x200 + 1 : rnd() % 0x30000 + 1);
         put(i, (sz ? flags : 0) | sz << 32 | near_addr());
      }

      begin    = near_addr();
      size     = (uint32_t)(rnd() % 2 ? rnd() % 0x100 + 1 : rnd() % 0x20000 + 1);
      is_share = rnd() % 3 ? 0 : (rnd() % 16) << 36;
      point    = (rnd() % 2 ? (uint64_t)near_addr() : ((rnd() % 16) << 36 | (rnd() % 0x10000))) / 128;

      for (kind = 0; kind < 3; kind++)
         if (new_scan((int)kind, mask, begin, size, is_share, point)
               != old_scan((int)kind, mask, begin, size, is_share, point))
            differ++;
   }

   TEST_CHECK(differ == 0);
   if (differ)
      fprintf(stderr, "scans differing from the old code: %u\n", differ);
}

#define THREADS 4
#define ROUNDS  100000

static retro_atomic_int_t holder[VM_RANGE_LOCKS];
static retro_atomic_int_t go, twice, none_free;

static void *taker(void *arg)
{
   const int me = (int)(size_t)arg + 1;
   int held[8];
   int i, k;

   while (!retro_atomic_load_acquire_int(&go))
      sched_yield();

   for (i = 0; i < ROUNDS; i++)
   {
      for (k = 0; k < 8; k++)
      {
         held[k] = vm_range_lock_alloc(&bits);
         if (held[k] < 0)
         {
            retro_atomic_fetch_add_int(&none_free, 1);
            continue;
         }
         if (!retro_atomic_cas_int(&holder[held[k]], 0, me))
            retro_atomic_fetch_add_int(&twice, 1);
      }

      for (k = 0; k < 8; k++)
      {
         if (held[k] < 0)
            continue;
         if (!retro_atomic_cas_int(&holder[held[k]], me, 0))
            retro_atomic_fetch_add_int(&twice, 1);
         vm_range_lock_free(&bits, set, (unsigned)held[k]);
      }
   }

   return NULL;
}

static void together(void)
{
   pthread_t t[THREADS];
   size_t i;

   clear_all();
   for (i = 0; i < THREADS; i++)
      pthread_create(&t[i], NULL, taker, (void*)i);
   retro_atomic_store_release_int(&go, 1);
   for (i = 0; i < THREADS; i++)
      pthread_join(t[i], NULL);

   TEST_CHECK(retro_atomic_load_acquire_int(&twice) == 0);
   /* At most 32 are held at once: there is always one free */
   TEST_CHECK(retro_atomic_load_acquire_int(&none_free) == 0);
   TEST_CHECK(retro_atomic_load_acquire_64(&bits) == 0);
}

/* The reader sets its slot and looks at the writers' bits; the writer sets
 * its bit and looks at the slot. Each round both go at once (a barrier per
 * round, the two threads kept alive), and at least one must see the
 * other */
#define DEKKER_ROUNDS 200000

static retro_atomic_64_t writers;
static retro_atomic_int_t step_of[2];
static uint64_t reader_saw[DEKKER_ROUNDS];
static uint64_t writer_saw[DEKKER_ROUNDS];

static void pin(int cpu)
{
#ifdef __linux__
   cpu_set_t c;
   long n = sysconf(_SC_NPROCESSORS_ONLN);
   if (n < 2)
      return;
   CPU_ZERO(&c);
   CPU_SET(cpu % n, &c);
   pthread_setaffinity_np(pthread_self(), sizeof(c), &c);
#else
   (void)cpu;
#endif
}

/* Both threads at @step before either goes on: three steps a round (go,
 * looked, cleared) */
static void meet(int me, int step)
{
   retro_atomic_store_release_int(&step_of[me], step);
   while (retro_atomic_load_acquire_int(&step_of[!me]) < step)
      ;
}

static void *dekker_reader(void *arg)
{
   int r;
   (void)arg;
   pin(0);
   for (r = 0; r < DEKKER_ROUNDS; r++)
   {
      meet(0, 3 * r + 1);
      vm_range_lock_publish(&set[0].word, 0x1000 | (uint64_t)0x80 << 32);
      reader_saw[r] = (uint64_t)retro_atomic_load_acquire_64(&writers);
      meet(0, 3 * r + 2);
      /* Both cleared by the reader, before either goes on */
      retro_atomic_store_release_64(&set[0].word, 0);
      retro_atomic_store_release_64(&writers, 0);
      meet(0, 3 * r + 3);
   }
   return NULL;
}

static void *dekker_writer(void *arg)
{
   int r;
   (void)arg;
   pin(1);
   for (r = 0; r < DEKKER_ROUNDS; r++)
   {
      meet(1, 3 * r + 1);
      retro_atomic_fetch_or_64(&writers, 2);
      vm_range_lock_fence_after_rmw();
      writer_saw[r] = (uint64_t)retro_atomic_load_acquire_64(&set[0].word);
      meet(1, 3 * r + 2);
      meet(1, 3 * r + 3);
   }
   return NULL;
}

static void both_see(void)
{
   pthread_t a, b;
   int r, missed = 0;

   clear_all();
   pthread_create(&a, NULL, dekker_reader, NULL);
   pthread_create(&b, NULL, dekker_writer, NULL);
   pthread_join(a, NULL);
   pthread_join(b, NULL);

   for (r = 0; r < DEKKER_ROUNDS; r++)
      if (!reader_saw[r] && !writer_saw[r])
         missed++;

   TEST_CHECK(missed == 0);
   if (missed)
      fprintf(stderr, "rounds where neither saw the other: %d\n", missed);
}

/* A thread waiting for the writers' bits to change sleeps on the wake
 * epoch; the writer clearing its bit must wake it. Each round the waiter
 * waits with a 1 s bound, so a release that forgets to wake it shows as an
 * expired wait (and not a hang) */
#define WAKE_ROUNDS 3000
static vm_writer_wake_t wake;
static retro_atomic_64_t wake_bits;
static retro_atomic_int_t wake_round, wake_done, wake_missed;

static void *wake_waiter(void *arg)
{
   int r;
   (void)arg;
   for (r = 1; r <= WAKE_ROUNDS; r++)
   {
      while (retro_atomic_load_acquire_int(&wake_round) != r)
         sched_yield();
      while (retro_atomic_load_acquire_64(&wake_bits))
      {
         const uint32_t e = vm_writer_wait_begin(&wake);
         if (retro_atomic_load_acquire_64(&wake_bits))
         {
            retro_waitaddr_t w;
            w.addr = &wake.epoch;
            w.old  = e;
            if (!retro_waitaddr_wait(&w, 1, 1000000))
            {
               retro_atomic_inc_int(&wake_missed);
               vm_writer_wait_end(&wake);
               retro_atomic_store_release_int(&wake_done, r);
               return NULL;
            }
         }
         vm_writer_wait_end(&wake);
      }
      retro_atomic_store_release_int(&wake_done, r);
   }
   return NULL;
}

static void woken(void)
{
   pthread_t t;
   int r;

   pthread_create(&t, NULL, wake_waiter, NULL);
   for (r = 1; r <= WAKE_ROUNDS && !retro_atomic_load_acquire_int(&wake_missed); r++)
   {
      retro_atomic_store_release_64(&wake_bits, 1);
      retro_atomic_store_release_int(&wake_round, r);
      /* Mostly release while it sleeps; sometimes right away */
      if (r & 1)
         while (!retro_atomic_load_acquire_int(&wake.waiters)
               && retro_atomic_load_acquire_int(&wake_done) != r)
            sched_yield();
      retro_atomic_fetch_and_64(&wake_bits, 0);
      vm_writer_released(&wake);
      while (retro_atomic_load_acquire_int(&wake_done) != r)
         sched_yield();
   }
   pthread_join(t, NULL);

   TEST_CHECK(retro_atomic_load_acquire_int(&wake_missed) == 0);
   TEST_CHECK(retro_atomic_load_acquire_int(&wake.waiters) == 0);
}

int main(void)
{
   alarm(600);
   alone();
   picked();
   random_sets();
   together();
   both_see();
   woken();
   printf("test_vm_range_locks: %s\n", test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
