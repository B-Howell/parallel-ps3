/* The lv2 scheduler's run queue with no lock and no turn (lv2_sched.h).
 * C89. */

#include <string.h>

#include "lv2_sched.h"

#define SC_PRIO_MASK  0xfffu
#define SC_QUEUED     ((uint32_t)1 << 12)
#define SC_TOLD       ((uint32_t)1 << 13)
#define SC_FIRST      ((uint32_t)1 << 14)
#define SC_EDGE       ((uint32_t)1 << 16)
#define SC_BUF_MASK   0xffu
#define SC_FREE_WORDS ((LV2_SCHED_BUFFERS + 63u) / 64u)
#define SC_NO_PRIO    0x7fffffff

#ifdef LV2_SCHED_TEST_WINDOW
/* Called between a change's read and its compare-and-swap: a test makes
 * another change there */
void LV2_SCHED_TEST_WINDOW(lv2_sched_t *s);
#endif

static void sc_set_bit(uint32_t m[4], unsigned slot)
{
   m[slot / 32u] |= (uint32_t)1 << (slot % 32u);
}

static void sc_write(lv2_sched_buf_t *b, const lv2_sched_view_t *v)
{
   unsigned i;

   retro_atomic_store_release_int(&b->head, (int)(v->count | (v->pending << 8)));
   retro_atomic_store_release_int(&b->others, (int)v->others);
   for (i = 0; i < LV2_SCHED_LIST_WORDS; i++)
   {
      uint32_t w = 0;
      unsigned j;
      for (j = 0; j < 4u && i * 4u + j < LV2_SCHED_THREADS; j++)
         w |= (uint32_t)v->list[i * 4u + j] << (j * 8u);
      retro_atomic_store_release_int(&b->list[i], (int)w);
   }
   for (i = 0; i < LV2_SCHED_THREADS; i++)
      retro_atomic_store_release_int(&b->thread[i], (int)v->thread[i]);
}

static void sc_copy(lv2_sched_buf_t *b, lv2_sched_view_t *v)
{
   unsigned i;
   const uint32_t head = (uint32_t)retro_atomic_load_acquire_int(&b->head);

   v->count   = head & 0xffu;
   v->pending = (head >> 8) & 0xffu;
   v->others  = (unsigned)retro_atomic_load_acquire_int(&b->others);
   for (i = 0; i < LV2_SCHED_LIST_WORDS; i++)
   {
      const uint32_t w = (uint32_t)retro_atomic_load_acquire_int(&b->list[i]);
      unsigned j;
      for (j = 0; j < 4u && i * 4u + j < LV2_SCHED_THREADS; j++)
         v->list[i * 4u + j] = (unsigned char)(w >> (j * 8u));
   }
   for (i = 0; i < LV2_SCHED_THREADS; i++)
      v->thread[i] = (uint32_t)retro_atomic_load_acquire_int(&b->thread[i]);
}

/* The snapshot in place into @v, and the word naming it. A buffer is only
 * written again once it is no longer in place, so a copy the word still
 * names afterwards is whole */
static uint64_t sc_read(lv2_sched_t *s, lv2_sched_view_t *v)
{
   for (;;)
   {
      const uint64_t c = (uint64_t)retro_atomic_load_acquire_64(&s->cur);
      sc_copy(&s->buf[c & SC_BUF_MASK], v);
      if ((uint64_t)retro_atomic_load_relaxed_64(&s->cur) == c)
         return c;
   }
}

/* One thread's word from the snapshot in place. The word is read with a
 * read-modify-write: the latest there is, after what came before (an
 * edge's putting told) */
static uint32_t sc_read_thread(lv2_sched_t *s, unsigned slot)
{
   for (;;)
   {
      const uint64_t c = (uint64_t)retro_atomic_fetch_add_64(&s->cur, 0);
      const uint32_t w = (uint32_t)retro_atomic_load_acquire_int(&s->buf[c & SC_BUF_MASK].thread[slot]);
      if ((uint64_t)retro_atomic_load_relaxed_64(&s->cur) == c)
         return w;
   }
}

/* A buffer no one has in place or in hand */
static unsigned sc_claim(lv2_sched_t *s)
{
   for (;;)
   {
      unsigned i;
      for (i = 0; i < SC_FREE_WORDS; i++)
      {
         const uint64_t f = (uint64_t)retro_atomic_load_acquire_64(&s->free[i]);
         if (f)
         {
            unsigned bit = 0;
            while (!((f >> bit) & 1u))
               bit++;
            if (retro_atomic_cas_64(&s->free[i], (int64_t)f, (int64_t)(f & ~((uint64_t)1 << bit))))
               return i * 64u + bit;
         }
      }
   }
}

static void sc_release(lv2_sched_t *s, unsigned b)
{
   retro_atomic_fetch_or_64(&s->free[b / 64u], (int64_t)((uint64_t)1 << (b % 64u)));
}

static void sc_fresh(lv2_sched_t *s, unsigned run_slots)
{
   lv2_sched_view_t v;
   unsigned i;

   memset(&v, 0, sizeof(v));
   s->run_slots = run_slots;
   for (i = 0; i < SC_FREE_WORDS; i++)
   {
      uint64_t f = 0;
      unsigned b;
      for (b = 0; b < 64u && i * 64u + b < LV2_SCHED_BUFFERS; b++)
         f |= (uint64_t)1 << b;
      retro_atomic_64_init(&s->free[i], (int64_t)f);
   }
   for (i = 0; i < LV2_SCHED_THREADS; i++)
      retro_atomic_int_init(&s->applied[i], 0);
   /* Buffer 0 in place */
   retro_atomic_64_init(&s->free[0], (int64_t)((uint64_t)retro_atomic_load_relaxed_64(&s->free[0]) & ~(uint64_t)1));
   sc_write(&s->buf[0], &v);
   retro_atomic_64_init(&s->cur, 0);
}

void lv2_sched_init(lv2_sched_t *s, unsigned run_slots)
{
   sc_fresh(s, run_slots);
}

void lv2_sched_reset(lv2_sched_t *s)
{
   sc_fresh(s, s->run_slots);
}

void lv2_sched_read(lv2_sched_t *s, lv2_sched_view_t *v)
{
   sc_read(s, v);
}

void lv2_sched_op_init(lv2_sched_op_t *op, int caller, int ready)
{
   memset(op, 0, sizeof(*op));
   op->sleeper         = -1;
   op->target          = -1;
   op->prio            = LV2_SCHED_NONE;
   op->caller          = caller;
   op->ready           = ready;
   op->lowest_new_prio = SC_NO_PRIO;
}

/* The work out of a change, on plain memory */

static int sc_find(const lv2_sched_view_t *v, unsigned slot)
{
   unsigned i;
   for (i = 0; i < v->count; i++)
   {
      if (v->list[i] == slot)
         return (int)i;
   }
   return -1;
}

static void sc_remove_at(lv2_sched_view_t *v, unsigned at)
{
   memmove(&v->list[at], &v->list[at + 1u], v->count - at - 1u);
   v->count--;
}

static int sc_prio(const lv2_sched_view_t *v, unsigned slot)
{
   return LV2_SCHED_PRIO(v->thread[slot]);
}

static void sc_set_prio(lv2_sched_view_t *v, unsigned slot, int prio)
{
   v->thread[slot] = (v->thread[slot] & ~(uint32_t)SC_PRIO_MASK) | ((uint32_t)prio & SC_PRIO_MASK);
}

/* Its next edge, and the change marks the thread for it to be put */
static void sc_edge(lv2_sched_view_t *v, unsigned slot, uint32_t edged[4])
{
   v->thread[slot] += SC_EDGE;
   sc_set_bit(edged, slot);
}

/* emplace_thread: in the run queue by priority, after its equals */
static int sc_emplace(lv2_sched_view_t *v, unsigned slot, lv2_sched_op_t *op)
{
   unsigned at;

   if (v->thread[slot] & SC_QUEUED)
   {
      sc_set_bit(op->found, slot);
      return 0;
   }

   for (at = 0; at < v->count && sc_prio(v, v->list[at]) <= sc_prio(v, slot); at++)
      ;
   memmove(&v->list[at + 1u], &v->list[at], v->count - at);
   v->list[at] = (unsigned char)slot;
   v->count++;
   v->thread[slot] |= SC_QUEUED;
   sc_set_bit(op->emplaced, slot);
   return 1;
}

/* awake_unlocked */
/* Every edge of @slot's up to its @edges put: its stop put to it, for one
 * told to stop to be taken as stopping */
static int sc_put(lv2_sched_t *s, unsigned slot, uint32_t word)
{
   const int a = retro_atomic_load_acquire_int(&s->applied[slot]);
   return !(a & 1) && (((unsigned)a >> 1) & 0xffffu) == LV2_SCHED_EDGES(word);
}

static int sc_awake(lv2_sched_t *s, lv2_sched_view_t *v, unsigned run_slots, lv2_sched_op_t *op, uint32_t edged[4])
{
   const int prio = op->prio;
   const int cpu  = op->target;
   int changed;
   unsigned i;

   if (prio >= 0)
   {
      const int old = sc_prio(v, (unsigned)cpu);

      /* The same priority: a thread setting its own keeps its place */
      if (old == prio && cpu == op->caller)
      {
         op->prio_order = 0;
         return 1;
      }

      if (!(v->thread[cpu] & SC_QUEUED))
      {
         sc_set_prio(v, (unsigned)cpu, prio);
         op->prio_order = old > prio ? 1 : old < prio ? -1 : 0;
         return 1;
      }

      sc_remove_at(v, (unsigned)sc_find(v, (unsigned)cpu));
      v->thread[cpu] &= ~SC_QUEUED;
      sc_set_prio(v, (unsigned)cpu, prio);
      op->prio_order = 0;
   }
   else if (prio == LV2_SCHED_YIELD)
   {
      const int at = cpu >= 0 ? sc_find(v, (unsigned)cpu) : -1;
      unsigned last;

      if (at < 0)
         return 0;

      /* To the back of its equals */
      for (last = (unsigned)at; last + 1u < v->count && sc_prio(v, v->list[last + 1u]) == sc_prio(v, (unsigned)cpu); last++)
         ;
      if (last == (unsigned)at)
         return 0;
      memmove(&v->list[at], &v->list[at + 1], last - (unsigned)at);
      v->list[last] = (unsigned char)cpu;

      /* Rotated, but within the run slots: no switch */
      if (last < run_slots)
         return 0;
      op->switched = 1;
   }

   changed           = prio == LV2_SCHED_YIELD;
   op->had_free_slot = v->count < run_slots;

   if (cpu >= 0 && prio != LV2_SCHED_YIELD)
   {
      if (sc_emplace(v, (unsigned)cpu, op))
      {
         changed = 1;
         if (sc_prio(v, (unsigned)cpu) < op->lowest_new_prio)
            op->lowest_new_prio = sc_prio(v, (unsigned)cpu);
      }
   }
   else
   {
      for (i = 0; i < op->to_awake_count; i++)
      {
         const unsigned t = op->to_awake[i];
         if (t < LV2_SCHED_THREADS && sc_emplace(v, t, op))
         {
            changed = 1;
            if (sc_prio(v, t) < op->lowest_new_prio)
               op->lowest_new_prio = sc_prio(v, t);
         }
      }
   }

   /* Past the run slots, the running told to stop */
   for (i = run_slots; i < v->count; i++)
   {
      const unsigned t = v->list[i];
      if (LV2_SCHED_LET_RUN(v->thread[t]))
      {
         v->thread[t] |= SC_TOLD;
         v->pending++;
         sc_edge(v, t, edged);
      }
   }

   /* The caller stopped, as it was told - once its stop is put to it, so
    * that it sees it as it goes back: a stop not yet put, or told in this
    * change, is taken once put (sc_commit) */
   if (op->caller >= 0 && (v->thread[op->caller] & SC_TOLD)
         && (op->stopped || sc_put(s, (unsigned)op->caller, v->thread[op->caller])))
   {
      v->thread[op->caller] &= ~SC_TOLD;
      v->pending--;
   }

   return changed;
}

static unsigned sc_first_sleeps(const lv2_sched_view_t *v)
{
   unsigned n = v->others;
   unsigned i;
   for (i = 0; i < LV2_SCHED_THREADS; i++)
      n += (unsigned)LV2_SCHED_FIRST(v->thread[i]);
   return n;
}

/* sleep_unlocked */
static int sc_sleep(lv2_sched_view_t *v, lv2_sched_op_t *op, uint32_t edged[4])
{
   const unsigned t = (unsigned)op->sleeper;
   int at;

   if (v->thread[t] & SC_TOLD)
   {
      v->thread[t] &= ~SC_TOLD;
      v->pending--;
   }

   if (op->signaled)
      return LV2_SCHED_NOT_SLEPT;

   at = sc_find(v, t);
   if (at < 0)
   {
      if (v->thread[t] & SC_FIRST)
      {
         v->thread[t] &= ~SC_FIRST;
         op->last_first_sleep = sc_first_sleeps(v) == 0;
         return LV2_SCHED_FIRST_SLEEP;
      }
      return LV2_SCHED_NOT_QUEUED;
   }

   sc_remove_at(v, (unsigned)at);
   v->thread[t] &= ~SC_QUEUED;
   /* It stops itself: its edge, for its turn among its others */
   if (LV2_SCHED_LET_RUN(v->thread[t]))
      sc_edge(v, t, edged);
   return LV2_SCHED_SLEPT;
}

/* schedule_all's waking: the first run slots let run, once every thread
 * told to stop has */
static void sc_schedule(lv2_sched_view_t *v, unsigned run_slots, const lv2_sched_op_t *op, uint32_t edged[4])
{
   unsigned i;

   if (v->pending || !op->ready)
      return;
   for (i = 0; i < run_slots && i < v->count; i++)
   {
      const unsigned t = v->list[i];
      if (!LV2_SCHED_LET_RUN(v->thread[t]))
         sc_edge(v, t, edged);
   }
}

/* Puts @slot's edges not yet put, in order, while no other is putting
 * one: the one that is will look again once it has put its own */
static void sc_apply(lv2_sched_t *s, unsigned slot, lv2_sched_edge_t edge, void *ctx)
{
   for (;;)
   {
      const int a        = retro_atomic_load_seq_cst_int(&s->applied[slot]);
      const unsigned done = (unsigned)a >> 1;
      unsigned next;

      if (a & 1)
         return;

      if (LV2_SCHED_EDGES(sc_read_thread(s, slot)) == (done & 0xffffu))
         return;

      next = (done + 1u) & 0xffffu;
      if (!retro_atomic_cas_int(&s->applied[slot], a, (int)((next << 1) | 1u)))
         continue;

      if (edge)
         edge(ctx, slot, (next & 1u) ? LV2_SCHED_RUN : LV2_SCHED_STOP);

      /* Put; then the snapshot looked at again (seq_cst both: a change
       * that saw this one putting sees it put, or it sees that change) */
      retro_atomic_exchange_int(&s->applied[slot], (int)(next << 1));
   }
}

void lv2_sched_apply(lv2_sched_t *s, unsigned slot, lv2_sched_edge_t edge, void *ctx)
{
   if (slot < LV2_SCHED_THREADS)
      sc_apply(s, slot, edge, ctx);
}

/* Works @op out from each snapshot until one stays in place for it */
static void sc_commit(lv2_sched_t *s, lv2_sched_op_t *op, lv2_sched_edge_t edge, void *ctx, int kind)
{
   const lv2_sched_op_t asked = *op;
   const unsigned mine = sc_claim(s);
   lv2_sched_view_t v;
   uint32_t edged[4];
   unsigned i;

   for (;;)
   {
      const uint64_t c = sc_read(s, &v);

      *op = asked;
      edged[0] = edged[1] = edged[2] = edged[3] = 0;

      switch (kind)
      {
      case 0:
         if (op->sleeper >= 0 && (unsigned)op->sleeper < LV2_SCHED_THREADS)
         {
            op->result = sc_sleep(&v, op, edged);
            if (op->to_awake_count)
            {
               lv2_sched_op_t inner = *op;
               inner.target = -1;
               inner.prio   = LV2_SCHED_ENQUEUE;
               sc_awake(s, &v, s->run_slots, &inner, edged);
               memcpy(op->emplaced, inner.emplaced, sizeof(op->emplaced));
               memcpy(op->found, inner.found, sizeof(op->found));
            }
         }
         else if (op->prio != LV2_SCHED_NONE && (op->target < 0 ? op->prio < 0 : (unsigned)op->target < LV2_SCHED_THREADS))
            op->changed = op->result = sc_awake(s, &v, s->run_slots, op, edged);
         sc_schedule(&v, s->run_slots, op, edged);
         break;
      case 1: /* a first sleep to come */
         if (op->target >= 0)
            v.thread[op->target] |= SC_FIRST;
         else
            v.others++;
         break;
      case 3: /* the caller's stop, put: taken as stopping */
         if (op->caller < 0 || !(v.thread[op->caller] & SC_TOLD) || !sc_put(s, (unsigned)op->caller, v.thread[op->caller]))
         {
            sc_release(s, mine);
            return;
         }
         v.thread[op->caller] &= ~SC_TOLD;
         v.pending--;
         sc_schedule(&v, s->run_slots, op, edged);
         break;
      case 2: /* another thread's first sleep */
         if (v.others)
         {
            v.others--;
            op->last_first_sleep = sc_first_sleeps(&v) == 0;
            op->result = 1;
         }
         break;
      }

#ifdef LV2_SCHED_TEST_WINDOW
      LV2_SCHED_TEST_WINDOW(s);
#endif

      sc_write(&s->buf[mine], &v);
      if (retro_atomic_cas_64(&s->cur, (int64_t)c, (int64_t)((((c >> 8) + 1u) << 8) | mine)))
      {
         sc_release(s, (unsigned)(c & SC_BUF_MASK));
         break;
      }
   }

   /* The edges this change made, put - or left to the one putting an
    * earlier edge of the thread's: the swap and the look at what is put
    * both sequentially consistent, against sc_apply's */
   for (i = 0; i < LV2_SCHED_THREADS; i++)
   {
      if ((edged[i / 32u] >> (i % 32u)) & 1u)
         sc_apply(s, i, edge, ctx);
   }

   /* The caller told to stop, its stop now put: it goes back to see it, so
    * it is taken as stopping, as the turn took it */
   if (kind == 0 && asked.caller >= 0 && (unsigned)asked.caller < LV2_SCHED_THREADS)
   {
      const uint32_t w = sc_read_thread(s, (unsigned)asked.caller);
      if ((w & SC_TOLD) && sc_put(s, (unsigned)asked.caller, w))
      {
         lv2_sched_op_t ack;
         lv2_sched_op_init(&ack, asked.caller, asked.ready);
         sc_commit(s, &ack, edge, ctx, 3);
      }
   }
}

void lv2_sched_change(lv2_sched_t *s, lv2_sched_op_t *op, lv2_sched_edge_t edge, void *ctx)
{
   sc_commit(s, op, edge, ctx, 0);
}

void lv2_sched_first_sleep(lv2_sched_t *s, int slot)
{
   lv2_sched_op_t op;
   lv2_sched_op_init(&op, -1, 0);
   op.target = slot >= 0 && (unsigned)slot < LV2_SCHED_THREADS ? slot : -1;
   sc_commit(s, &op, NULL, NULL, 1);
}

int lv2_sched_other_slept(lv2_sched_t *s)
{
   lv2_sched_op_t op;
   lv2_sched_op_init(&op, -1, 0);
   sc_commit(s, &op, NULL, NULL, 2);
   return op.result && op.last_first_sleep;
}

unsigned lv2_sched_first_sleeps(lv2_sched_t *s)
{
   lv2_sched_view_t v;
   sc_read(s, &v);
   return sc_first_sleeps(&v);
}

int lv2_sched_place(lv2_sched_t *s, unsigned slot)
{
   lv2_sched_view_t v;
   sc_read(s, &v);
   return slot < LV2_SCHED_THREADS ? sc_find(&v, slot) : -1;
}

unsigned lv2_sched_count(lv2_sched_t *s)
{
   for (;;)
   {
      const uint64_t c = (uint64_t)retro_atomic_load_acquire_64(&s->cur);
      const uint32_t h = (uint32_t)retro_atomic_load_acquire_int(&s->buf[c & SC_BUF_MASK].head);
      if ((uint64_t)retro_atomic_load_relaxed_64(&s->cur) == c)
         return h & 0xffu;
   }
}

int lv2_sched_at(lv2_sched_t *s, unsigned index)
{
   lv2_sched_view_t v;
   sc_read(s, &v);
   return index < v.count ? (int)v.list[index] : -1;
}
