/* An lv2 rwlock's owners and waiters with no lock (lv2_rwq.h). C89. */

#include "lv2_rwq.h"

#ifdef LV2_RWQ_TEST_WINDOW
void LV2_RWQ_TEST_WINDOW(lv2_rwq_t *q);
#endif

#define RQ_ONE          ((uint64_t)1)
#define RQ_HI_SLOTS     ((RQ_ONE << (LV2_RWQ_SLOTS - 64u)) - 1u)
#define RQ_READER_SHIFT 36u
#define RQ_READER_MASK  ((uint64_t)LV2_RWQ_MAX_READERS << RQ_READER_SHIFT)
#define RQ_WRITER_SHIFT 56u
#define RQ_WRITER_MASK  ((uint64_t)0x7f << RQ_WRITER_SHIFT)

/* Readers counted while those waiting are taken off their set: as many as
 * it has slots */
#define RQ_RESERVE      LV2_WAITSET_SLOTS

static int rq_writer(retro_u128_t w)
{
   return (int)((w.hi & RQ_WRITER_MASK) >> RQ_WRITER_SHIFT) - 1;
}

static retro_u128_t rq_with_writer(retro_u128_t w, int writer)
{
   w.hi = (w.hi & ~RQ_WRITER_MASK) | ((uint64_t)(writer + 1) << RQ_WRITER_SHIFT);
   return w;
}

static unsigned rq_readers(retro_u128_t w)
{
   return (unsigned)((w.hi & RQ_READER_MASK) >> RQ_READER_SHIFT);
}

static retro_u128_t rq_with_readers(retro_u128_t w, unsigned readers)
{
   w.hi = (w.hi & ~RQ_READER_MASK) | ((uint64_t)readers << RQ_READER_SHIFT);
   return w;
}

static int rq_bit(retro_u128_t w, unsigned slot)
{
   return slot < 64u ? (int)((w.lo >> slot) & 1u) : (int)((w.hi >> (slot - 64u)) & 1u);
}

static retro_u128_t rq_with(retro_u128_t w, unsigned slot, int set)
{
   uint64_t *half   = slot < 64u ? &w.lo : &w.hi;
   const uint64_t m = RQ_ONE << (slot % 64u);
   if (set)
      *half |= m;
   else
      *half &= ~m;
   return w;
}

static void rq_mark(retro_u128_t *v, unsigned slot)
{
   if (slot < 64u)
      v->lo |= RQ_ONE << slot;
   else
      v->hi |= RQ_ONE << (slot - 64u);
}

/* Writers waiting */
static int rq_any(retro_u128_t w)
{
   return w.lo || (w.hi & RQ_HI_SLOTS);
}

/* Nothing holds readers off: no writer holds it or waits for it */
static int rq_open(retro_u128_t w)
{
   return rq_writer(w) < 0 && !rq_any(w);
}

static unsigned rq_popcount(uint64_t v)
{
   unsigned n = 0;
   while (v)
   {
      v &= v - 1u;
      n++;
   }
   return n;
}

void lv2_rwq_init(lv2_rwq_t *q, int writer, unsigned readers)
{
   retro_u128_t w;
   unsigned i;

   w.lo = 0;
   w.hi = 0;
   if (writer >= 0 && (unsigned)writer < LV2_RWQ_SLOTS)
      w = rq_with_writer(w, writer);
   else if (readers <= LV2_RWQ_MAX_READERS)
      w = rq_with_readers(w, readers);
   retro_atomic_128_init(&q->word, w);
   retro_atomic_int_init(&q->next_ticket, 0);
   for (i = 0; i < LV2_RWQ_SLOTS; i++)
      retro_atomic_int_init(&q->ticket[i], 0);
   lv2_waitset_init(&q->readers);
}

/* The word as it is to be after a change that may leave no writer: the
 * best writer waiting given it when no reader holds it (set in @woken) */
static retro_u128_t rq_settle(lv2_rwq_t *q, retro_u128_t w, lv2_rwq_rank_t rank, void *ctx,
      retro_u128_t *woken)
{
   int best      = -1;
   int best_rank = 0;
   int best_tick = 0;
   unsigned s;

   if (rq_writer(w) >= 0 || rq_readers(w) || !rq_any(w))
      return w;

   for (s = 0; s < LV2_RWQ_SLOTS; s++)
   {
      int r;
      int t;

      if (!rq_bit(w, s))
         continue;

      r = rank ? rank(ctx, s) : 0;
      t = retro_atomic_load_acquire_int(&q->ticket[s]);
      /* Tickets compared by their difference: they wrap */
      if (best < 0 || r < best_rank
            || (r == best_rank && (int)((unsigned)t - (unsigned)best_tick) < 0))
      {
         best      = (int)s;
         best_rank = r;
         best_tick = t;
      }
   }

   rq_mark(woken, (unsigned)best);
   return rq_with_writer(rq_with(w, (unsigned)best, 0), best);
}

/* After a change that left the word open (@w): every reader waiting given
 * it, in three changes - readers counted for every slot of their set, so
 * no writer takes it meanwhile; those waiting taken off it; the count
 * made theirs, and the best writer given it if that leaves none */
static void rq_let_readers(lv2_rwq_t *q, retro_u128_t w, lv2_rwq_rank_t rank, void *ctx,
      retro_u128_t *woken)
{
   retro_u128_t taken;
   unsigned n;

   /* The word changed before the set is looked at: a reader joining after
    * looks at the word again itself */
   lv2_waitset_fence();
   if (!rq_open(w) || !lv2_waitset_any(&q->readers))
      return;

   for (;;)
   {
      const unsigned readers = rq_readers(w);

      /* A writer came meanwhile: it lets them in when it is done */
      if (!rq_open(w) || readers > LV2_RWQ_MAX_READERS - RQ_RESERVE)
         return;
      if (retro_atomic_cas_128(&q->word, w, rq_with_readers(w, readers + RQ_RESERVE)))
         break;
      w = retro_atomic_load_acquire_128(&q->word);
   }

   taken = lv2_waitset_claim_all(&q->readers);
   n     = rq_popcount(taken.lo) + rq_popcount(taken.hi);
   woken->lo |= taken.lo;
   woken->hi |= taken.hi;

   for (;;)
   {
      unsigned readers;
      retro_u128_t given;
      retro_u128_t next;

      w       = retro_atomic_load_acquire_128(&q->word);
      readers = rq_readers(w);
      /* (Fewer only when a thread gave up a read it never had) */
      readers = readers > RQ_RESERVE - n ? readers - (RQ_RESERVE - n) : 0u;
      given.lo = 0;
      given.hi = 0;
      next     = rq_settle(q, rq_with_readers(w, readers), rank, ctx, &given);
      if (retro_atomic_cas_128(&q->word, w, next))
      {
         woken->lo |= given.lo;
         woken->hi |= given.hi;
         return;
      }
   }
}

int lv2_rwq_tryrlock(lv2_rwq_t *q)
{
   for (;;)
   {
      const retro_u128_t w   = retro_atomic_load_acquire_128(&q->word);
      const unsigned readers = rq_readers(w);

      if (!rq_open(w) || readers >= LV2_RWQ_MAX_READERS)
         return LV2_RWQ_BUSY;
      if (retro_atomic_cas_128(&q->word, w, rq_with_readers(w, readers + 1u)))
         return LV2_RWQ_OWNED;
   }
}

int lv2_rwq_rlock(lv2_rwq_t *q, unsigned slot)
{
   if (slot >= LV2_RWQ_SLOTS)
      return LV2_RWQ_BAD;

   for (;;)
   {
      retro_u128_t w = retro_atomic_load_acquire_128(&q->word);

      if (rq_open(w))
      {
         if (rq_readers(w) >= LV2_RWQ_MAX_READERS)
            return LV2_RWQ_BAD;
         if (retro_atomic_cas_128(&q->word, w, rq_with_readers(w, rq_readers(w) + 1u)))
            return LV2_RWQ_OWNED;
         continue;
      }

#ifdef LV2_RWQ_TEST_WINDOW
      /* The test gives the lock up here: an unlock that saw no reader */
      LV2_RWQ_TEST_WINDOW(q);
#endif

      if (!lv2_waitset_join(&q->readers, slot))
         return LV2_RWQ_BAD;

      /* The word again, after joining: an unlock before it did not see it */
      w = retro_atomic_load_acquire_128(&q->word);
      if (!rq_open(w))
         return LV2_RWQ_QUEUED;
      if (!lv2_waitset_leave(&q->readers, slot))
         return LV2_RWQ_QUEUED; /* given it already */
   }
}

int lv2_rwq_wlock(lv2_rwq_t *q, unsigned slot)
{
   int ticket;

   if (slot >= LV2_RWQ_SLOTS)
      return LV2_RWQ_BAD;

   ticket = retro_atomic_fetch_add_int(&q->next_ticket, 1);

   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      const int writer     = rq_writer(w);

      if (writer == (int)slot)
         return LV2_RWQ_MINE;
      if (writer < 0 && !rq_readers(w) && !rq_any(w))
      {
         if (retro_atomic_cas_128(&q->word, w, rq_with_writer(w, (int)slot)))
            return LV2_RWQ_OWNED;
         continue;
      }

      if (rq_bit(w, slot))
         return LV2_RWQ_BAD;

      /* The ticket before the bit: an unlock that sees the bit reads it */
      retro_atomic_store_release_int(&q->ticket[slot], ticket);
      if (retro_atomic_cas_128(&q->word, w, rq_with(w, slot, 1)))
         return LV2_RWQ_QUEUED;
   }
}

int lv2_rwq_trywlock(lv2_rwq_t *q, unsigned slot)
{
   if (slot >= LV2_RWQ_SLOTS)
      return LV2_RWQ_BAD;

   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      const int writer     = rq_writer(w);

      if (writer == (int)slot)
         return LV2_RWQ_MINE;
      if (writer >= 0 || rq_readers(w) || rq_any(w))
         return LV2_RWQ_BUSY;
      if (retro_atomic_cas_128(&q->word, w, rq_with_writer(w, (int)slot)))
         return LV2_RWQ_OWNED;
   }
}

int lv2_rwq_runlock(lv2_rwq_t *q, lv2_rwq_rank_t rank, void *ctx, retro_u128_t *woken)
{
   woken->lo = 0;
   woken->hi = 0;

   for (;;)
   {
      const retro_u128_t w   = retro_atomic_load_acquire_128(&q->word);
      const unsigned readers = rq_readers(w);
      retro_u128_t given;
      retro_u128_t next;

      if (!readers || rq_writer(w) >= 0)
         return LV2_RWQ_NOT_OWNER;

      given.lo = 0;
      given.hi = 0;
      next     = rq_settle(q, rq_with_readers(w, readers - 1u), rank, ctx, &given);
      if (retro_atomic_cas_128(&q->word, w, next))
      {
         *woken = given;
         rq_let_readers(q, next, rank, ctx, woken);
         return 0;
      }
   }
}

int lv2_rwq_wunlock(lv2_rwq_t *q, unsigned slot, lv2_rwq_rank_t rank, void *ctx, retro_u128_t *woken)
{
   woken->lo = 0;
   woken->hi = 0;

   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      retro_u128_t given;
      retro_u128_t next;

      if (rq_writer(w) != (int)slot)
         return LV2_RWQ_NOT_OWNER;

      given.lo = 0;
      given.hi = 0;
      next     = rq_settle(q, rq_with_writer(w, -1), rank, ctx, &given);
      if (retro_atomic_cas_128(&q->word, w, next))
      {
         *woken = given;
         rq_let_readers(q, next, rank, ctx, woken);
         return 0;
      }
   }
}

int lv2_rwq_rleave(lv2_rwq_t *q, unsigned slot)
{
   return slot < LV2_RWQ_SLOTS && lv2_waitset_leave(&q->readers, slot);
}

int lv2_rwq_wleave(lv2_rwq_t *q, unsigned slot, lv2_rwq_rank_t rank, void *ctx, retro_u128_t *woken)
{
   woken->lo = 0;
   woken->hi = 0;

   if (slot >= LV2_RWQ_SLOTS)
      return 0;

   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
      retro_u128_t given;
      retro_u128_t next;

      if (!rq_bit(w, slot))
         return 0;

      given.lo = 0;
      given.hi = 0;
      next     = rq_settle(q, rq_with(w, slot, 0), rank, ctx, &given);
      if (retro_atomic_cas_128(&q->word, w, next))
      {
         /* The last writer waiting gone: the readers behind it let in */
         *woken = given;
         rq_let_readers(q, next, rank, ctx, woken);
         return 1;
      }
   }
}

int lv2_rwq_writer(lv2_rwq_t *q)
{
   return rq_writer(retro_atomic_load_acquire_128(&q->word));
}

unsigned lv2_rwq_readers(lv2_rwq_t *q)
{
   return rq_readers(retro_atomic_load_acquire_128(&q->word));
}

int lv2_rwq_waits(lv2_rwq_t *q, unsigned slot)
{
   return slot < LV2_RWQ_SLOTS && (rq_bit(retro_atomic_load_acquire_128(&q->word), slot)
         || lv2_waitset_has(&q->readers, slot));
}

int lv2_rwq_any_waiter(lv2_rwq_t *q)
{
   return rq_any(retro_atomic_load_acquire_128(&q->word)) || lv2_waitset_any(&q->readers);
}

int lv2_rwq_busy(lv2_rwq_t *q)
{
   const retro_u128_t w = retro_atomic_load_acquire_128(&q->word);
   lv2_waitset_fence();
   return w.lo || w.hi || lv2_waitset_any(&q->readers);
}
