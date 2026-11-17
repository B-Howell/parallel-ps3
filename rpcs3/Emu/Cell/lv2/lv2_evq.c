/* An lv2 event queue's events and waiters, lock-free (lv2_evq.h). C89. */

#include "lv2_evq.h"

#include <rthreads/retro_waitaddr.h>

#ifdef LV2_EVQ_TEST_WINDOW
void LV2_EVQ_TEST_WINDOW(lv2_evq_t *q);
#endif

#define EQ_ONE ((uint64_t)1)

static int eq_bit(const retro_u128_t *w, unsigned s)
{
   return s < 64u ? (int)((w->lo >> s) & 1u) : (int)((w->hi >> (s - 64u)) & 1u);
}

static retro_u128_t eq_with(retro_u128_t w, unsigned s, int set)
{
   uint64_t *half   = s < 64u ? &w.lo : &w.hi;
   const uint64_t m = EQ_ONE << (s % 64u);
   if (set)
      *half |= m;
   else
      *half &= ~m;
   return w;
}

static void eq_fence(void)
{
   retro_atomic_thread_fence_seq_cst();
}

/* A thread watching is told the queue changed */
static void eq_changed(lv2_evq_t *q)
{
   retro_atomic_fetch_add_int(&q->changes, 1);
   eq_fence();
   if (retro_atomic_load_acquire_int(&q->watchers))
      retro_waitaddr_notify_all((const void*)&q->changes);
}

int lv2_evq_init(lv2_evq_t *q, int size)
{
   retro_u128_t zero;
   unsigned i;

   zero.lo = 0;
   zero.hi = 0;
   if (!lv2_event_ring_init(&q->ring, size))
      return 0;
   retro_atomic_128_init(&q->waiting[0], zero);
   retro_atomic_128_init(&q->waiting[1], zero);
   retro_atomic_int_init(&q->next_ticket, 0);
   for (i = 0; i < LV2_EVQ_SLOTS; i++)
   {
      retro_atomic_int_init(&q->ticket[i], 0);
      retro_atomic_int_init(&q->timed_out[i], 0);
      retro_atomic_int_init(&q->verdict[i], 0);
   }
   retro_atomic_int_init(&q->claiming, 0);
   retro_atomic_int_init(&q->changes, 0);
   retro_atomic_int_init(&q->watchers, 0);
   return 1;
}

void lv2_evq_deinit(lv2_evq_t *q)
{
   lv2_event_ring_deinit(&q->ring);
}

/* The bit of @slot set (1, after its ticket) or cleared (1 when this
 * cleared it) */
static int eq_join(lv2_evq_t *q, unsigned slot, int ticket)
{
   retro_atomic_128_t *word = &q->waiting[slot / 128u];
   const unsigned s         = slot % 128u;

   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(word);
      if (eq_bit(&w, s))
         return 0;
      /* The ticket before the bit: a send that sees the bit reads it */
      retro_atomic_store_release_int(&q->ticket[slot], ticket);
      if (retro_atomic_cas_128(word, w, eq_with(w, s, 1)))
         break;
   }

   eq_fence();
   eq_changed(q);
   return 1;
}

static int eq_clear(lv2_evq_t *q, unsigned slot)
{
   retro_atomic_128_t *word = &q->waiting[slot / 128u];
   const unsigned s         = slot % 128u;

   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(word);
      if (!eq_bit(&w, s))
         return 0;
      if (retro_atomic_cas_128(word, w, eq_with(w, s, 0)))
         return 1;
   }
}

int lv2_evq_try(lv2_evq_t *q, uint64_t ev[4])
{
   if (!lv2_event_ring_pop(&q->ring, ev))
      return 0;
   eq_changed(q);
   return 1;
}

/* An event published at the ring's head: one a receive would take */
static int eq_ready(lv2_evq_t *q)
{
   const uint64_t pos           = (uint64_t)retro_atomic_load_acquire_64(&q->ring.head);
   const lv2_event_cell_t *cell = &q->ring.cells[pos & q->ring.mask];
   return (uint64_t)retro_atomic_load_acquire_64((retro_atomic_64_t*)&cell->seq) == pos + 1u;
}

int lv2_evq_wait(lv2_evq_t *q, unsigned slot, uint64_t ev[4])
{
   int ticket;

   if (slot >= LV2_EVQ_SLOTS)
      return LV2_EVQ_BAD;

   if (lv2_evq_try(q, ev))
      return LV2_EVQ_GOT;

   retro_atomic_store_release_int(&q->timed_out[slot], 0);
   retro_atomic_store_release_int(&q->verdict[slot], LV2_EVQ_PENDING);
   ticket = retro_atomic_fetch_add_int(&q->next_ticket, 1);

#ifdef LV2_EVQ_TEST_WINDOW
   /* The test sends here: a send that saw no waiter */
   LV2_EVQ_TEST_WINDOW(q);
#endif

   if (!eq_join(q, slot, ticket))
      return LV2_EVQ_BAD;

   /* The ring again, after joining: a send before it did not see it */
   for (;;)
   {
      if (!eq_ready(q))
         return LV2_EVQ_WAITING;
      if (!eq_clear(q, slot))
         return LV2_EVQ_WAITING; /* claimed: its verdict comes */
      if (lv2_evq_try(q, ev))
         return LV2_EVQ_GOT;
      /* Taken meanwhile: back, in its place */
      eq_join(q, slot, retro_atomic_load_acquire_int(&q->ticket[slot]));
   }
}

int lv2_evq_leave(lv2_evq_t *q, unsigned slot)
{
   return slot < LV2_EVQ_SLOTS && eq_clear(q, slot);
}

void lv2_evq_give(lv2_evq_t *q, unsigned slot, int verdict)
{
   retro_atomic_store_release_int(&q->verdict[slot], verdict);
   eq_fence();
   if (retro_atomic_load_acquire_int(&q->timed_out[slot]))
      retro_waitaddr_notify_all((const void*)&q->verdict[slot]);
}

int lv2_evq_verdict(lv2_evq_t *q, unsigned slot)
{
   return retro_atomic_load_acquire_int(&q->verdict[slot]);
}

int lv2_evq_await(lv2_evq_t *q, unsigned slot)
{
   retro_atomic_store_release_int(&q->timed_out[slot], 1);
   eq_fence();

   for (;;)
   {
      const int verdict = retro_atomic_load_acquire_int(&q->verdict[slot]);
      retro_waitaddr_t w;

      if (verdict != LV2_EVQ_PENDING)
         return verdict;

      w.addr = (const void*)&q->verdict[slot];
      w.old  = (uint32_t)LV2_EVQ_PENDING;
      retro_waitaddr_wait(&w, 1, -1);
   }
}

int lv2_evq_timeout(lv2_evq_t *q, unsigned slot)
{
   if (slot >= LV2_EVQ_SLOTS)
      return LV2_EVQ_TIMEDOUT;

   /* Its time-out told before it leaves: a send that claimed it first
    * reads it after giving its verdict, and wakes it */
   retro_atomic_store_release_int(&q->timed_out[slot], 1);
   eq_fence();

   if (eq_clear(q, slot))
   {
      retro_atomic_store_release_int(&q->verdict[slot], LV2_EVQ_TIMEDOUT);
      return LV2_EVQ_TIMEDOUT;
   }

   return lv2_evq_await(q, slot);
}

/* The best waiter: by rank, then by ticket (compared by difference: they
 * wrap). -1 when none */
static int eq_best(lv2_evq_t *q, lv2_evq_rank_t rank, void *ctx)
{
   retro_u128_t w[2];
   int best      = -1;
   int best_rank = 0;
   int best_tick = 0;
   unsigned slot;

   w[0] = retro_atomic_load_acquire_128(&q->waiting[0]);
   w[1] = retro_atomic_load_acquire_128(&q->waiting[1]);

   if (!w[0].lo && !w[0].hi && !w[1].lo && !w[1].hi)
      return -1;

   for (slot = 0; slot < LV2_EVQ_SLOTS; slot++)
   {
      int r;
      int t;

      if (!eq_bit(&w[slot / 128u], slot % 128u))
         continue;
      r = rank ? rank(ctx, slot) : 0;
      t = retro_atomic_load_acquire_int(&q->ticket[slot]);
      if (best < 0 || r < best_rank
            || (r == best_rank && (int)((unsigned)t - (unsigned)best_tick) < 0))
      {
         best      = (int)slot;
         best_rank = r;
         best_tick = t;
      }
   }
   return best;
}

int lv2_evq_send(lv2_evq_t *q, uint64_t ev[4], lv2_evq_rank_t rank, void *ctx, unsigned *slot)
{
   /* Claiming from here: a destroy that looks now is refused */
   retro_atomic_fetch_add_int(&q->claiming, 1);

   if (!lv2_event_ring_push(&q->ring, ev))
   {
      retro_atomic_fetch_sub_int(&q->claiming, 1);
      return LV2_EVQ_FULL;
   }

   /* The event before the waiters: a waiter that joined after this sees it
    * itself */
   eq_fence();

   for (;;)
   {
      const int best = eq_best(q, rank, ctx);
      unsigned s;

      if (best < 0)
         break;
      s = (unsigned)best;

      /* Claimed, or gone (left, or another send has it): the next */
      if (!eq_clear(q, s))
         continue;

      if (lv2_evq_try(q, ev))
      {
         *slot = s;
         retro_atomic_fetch_sub_int(&q->claiming, 1);
         return LV2_EVQ_HANDED;
      }

      /* A receive took it meanwhile. A waiter whose time ran out is given
       * it; else it is put back, in its place */
      if (retro_atomic_load_acquire_int(&q->timed_out[s]))
         lv2_evq_give(q, s, LV2_EVQ_TIMEDOUT);
      else
      {
         eq_join(q, s, retro_atomic_load_acquire_int(&q->ticket[s]));

         /* Its time ran out as it was put back: given it here, unless it
          * left on its own */
         if (retro_atomic_load_acquire_int(&q->timed_out[s]) && eq_clear(q, s))
            lv2_evq_give(q, s, LV2_EVQ_TIMEDOUT);
      }
      break;
   }

   retro_atomic_fetch_sub_int(&q->claiming, 1);
   return LV2_EVQ_SENT;
}

unsigned lv2_evq_claim_all(lv2_evq_t *q, retro_u128_t woken[2])
{
   retro_u128_t zero;
   unsigned count = 0;
   unsigned i;

   zero.lo = 0;
   zero.hi = 0;
   woken[0] = retro_atomic_exchange_128(&q->waiting[0], zero);
   woken[1] = retro_atomic_exchange_128(&q->waiting[1], zero);

   for (i = 0; i < LV2_EVQ_SLOTS; i++)
      if (eq_bit(&woken[i / 128u], i % 128u))
         count++;
   return count;
}

int lv2_evq_count(lv2_evq_t *q)
{
   return lv2_event_ring_count(&q->ring);
}

int lv2_evq_any_waiter(lv2_evq_t *q)
{
   const retro_u128_t a = retro_atomic_load_acquire_128(&q->waiting[0]);
   const retro_u128_t b = retro_atomic_load_acquire_128(&q->waiting[1]);
   return a.lo || a.hi || b.lo || b.hi;
}

int lv2_evq_waits(lv2_evq_t *q, unsigned slot)
{
   retro_u128_t w;
   if (slot >= LV2_EVQ_SLOTS)
      return 0;
   w = retro_atomic_load_acquire_128(&q->waiting[slot / 128u]);
   return eq_bit(&w, slot % 128u);
}

int lv2_evq_busy(lv2_evq_t *q)
{
   eq_fence();
   return lv2_evq_any_waiter(q) || retro_atomic_load_acquire_int(&q->claiming) != 0;
}

void lv2_evq_drain(lv2_evq_t *q)
{
   uint64_t ev[4];
   while (lv2_evq_try(q, ev))
      ;
}

void lv2_evq_each(lv2_evq_t *q, lv2_event_ring_each_t fn, void *ctx)
{
   lv2_event_ring_each(&q->ring, fn, ctx);
}

void lv2_evq_poke(lv2_evq_t *q)
{
   eq_changed(q);
}

unsigned lv2_evq_watch(lv2_evq_t *q)
{
   retro_atomic_fetch_add_int(&q->watchers, 1);
   eq_fence();
   return (unsigned)retro_atomic_load_acquire_int(&q->changes);
}

void lv2_evq_watch_wait(lv2_evq_t *q, unsigned seen, int64_t us)
{
   retro_waitaddr_t w;
   w.addr = (const void*)&q->changes;
   w.old  = (uint32_t)seen;
   retro_waitaddr_wait(&w, 1, us);
}

void lv2_evq_unwatch(lv2_evq_t *q)
{
   retro_atomic_fetch_sub_int(&q->watchers, 1);
}
