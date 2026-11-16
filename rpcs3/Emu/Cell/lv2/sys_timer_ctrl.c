/* An lv2 timer's state, lock-free (sys_timer_ctrl.h). C89. */

#include "sys_timer_ctrl.h"

#define CTRL_STATE(hi)       ((unsigned)((hi) & 3u))
#define CTRL_NEXT(hi, state) ((((hi) >> 2) + 1u) << 2 | (uint64_t)(state))

static uint64_t ctrl_add_saturate(uint64_t a, uint64_t b)
{
   const uint64_t r = a + b;
   return r < a ? UINT64_MAX : r;
}

static retro_u128_t ctrl_value(uint64_t expire, uint64_t hi)
{
   retro_u128_t v;
   v.lo = expire;
   v.hi = hi;
   return v;
}

void lv2_timer_ctrl_init(lv2_timer_ctrl_t *c, unsigned running,
      uint64_t expire, uint64_t period)
{
   const retro_u128_t v = ctrl_value(running ? expire : 0,
         running ? LV2_TIMER_CTRL_RUN : LV2_TIMER_CTRL_STOP);
   retro_atomic_128_init(&c->word, v);
   retro_atomic_64_init(&c->period, (int64_t)(running ? period : 0));
}

uint64_t lv2_timer_ctrl_check(lv2_timer_ctrl_t *c, uint64_t now,
      lv2_timer_ctrl_send_t send, void *ctx)
{
   for (;;)
   {
      retro_u128_t w = retro_atomic_load_acquire_128(&c->word);
      retro_u128_t firing;
      uint64_t period;
      uint64_t next;

      if (CTRL_STATE(w.hi) != LV2_TIMER_CTRL_RUN)
         return UINT64_MAX;
      if (now < w.lo)
         return w.lo - now;

      /* Claimed: this thread fires it, and nobody else */
      firing = ctrl_value(w.lo, CTRL_NEXT(w.hi, LV2_TIMER_CTRL_FIRING));
      if (!retro_atomic_cas_128(&c->word, w, firing))
         continue;

      if (send)
         send(ctx, w.lo);

      period = (uint64_t)retro_atomic_load_acquire_64(&c->period);
      if (!period)
      {
         /* A one shot stops; a stop meanwhile did already */
         (void)retro_atomic_cas_128(&c->word, firing,
               ctrl_value(w.lo, CTRL_NEXT(firing.hi, LV2_TIMER_CTRL_STOP)));
         return UINT64_MAX;
      }

      next = ctrl_add_saturate(w.lo, period);
      if (!retro_atomic_cas_128(&c->word, firing,
               ctrl_value(next, CTRL_NEXT(firing.hi, LV2_TIMER_CTRL_RUN))))
         return UINT64_MAX; /* stopped meanwhile */
      if (now < next)
         return next - now;
      /* Due again already: around once more */
   }
}

int lv2_timer_ctrl_start(lv2_timer_ctrl_t *c, uint64_t now, uint64_t base,
      uint64_t period, lv2_timer_ctrl_send_t send, void *ctx)
{
   (void)lv2_timer_ctrl_check(c, now, send, ctx);

   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&c->word);
      retro_u128_t setup;
      uint64_t expire;

      if (CTRL_STATE(w.hi) != LV2_TIMER_CTRL_STOP)
         return LV2_TIMER_CTRL_BUSY;
      if (!period && now >= base)
         return LV2_TIMER_CTRL_TIMED_OUT;

      if (!period)
         expire = base;                                 /* one shot */
      else if (!base)
         expire = ctrl_add_saturate(now, period);       /* periodic from now */
      else if (now < ctrl_add_saturate(base, period))
         expire = ctrl_add_saturate(base, period);      /* the first period after base */
      else
      {
         /* The first period boundary after now, counted from base (LV2
          * steps base up a period at a time until it is past) */
         const uint64_t aligned = ctrl_add_saturate(now - now % period, base % period);
         expire = ctrl_add_saturate(aligned, aligned < now ? period : 0);
      }

      /* Claimed for setting up: the period is this thread's to write */
      setup = ctrl_value(expire, CTRL_NEXT(w.hi, LV2_TIMER_CTRL_SETUP));
      if (!retro_atomic_cas_128(&c->word, w, setup))
         continue;

      retro_atomic_store_release_64(&c->period, (int64_t)period);

      /* Running; a stop meanwhile wins, as if it came after */
      (void)retro_atomic_cas_128(&c->word, setup,
            ctrl_value(expire, CTRL_NEXT(setup.hi, LV2_TIMER_CTRL_RUN)));
      return LV2_TIMER_CTRL_STARTED;
   }
}

void lv2_timer_ctrl_stop(lv2_timer_ctrl_t *c)
{
   for (;;)
   {
      const retro_u128_t w = retro_atomic_load_acquire_128(&c->word);

      if (CTRL_STATE(w.hi) == LV2_TIMER_CTRL_STOP)
         return;
      if (retro_atomic_cas_128(&c->word, w,
               ctrl_value(w.lo, CTRL_NEXT(w.hi, LV2_TIMER_CTRL_STOP))))
         return;
   }
}

void lv2_timer_ctrl_info(lv2_timer_ctrl_t *c, unsigned *running,
      uint64_t *expire, uint64_t *period)
{
   const retro_u128_t w = retro_atomic_load_acquire_128(&c->word);
   const unsigned state = CTRL_STATE(w.hi);

   if (state == LV2_TIMER_CTRL_RUN || state == LV2_TIMER_CTRL_FIRING)
   {
      *running = 1;
      *expire  = w.lo;
      *period  = (uint64_t)retro_atomic_load_acquire_64(&c->period);
      return;
   }

   *running = 0;
   *expire  = 0;
   *period  = 0;
}
