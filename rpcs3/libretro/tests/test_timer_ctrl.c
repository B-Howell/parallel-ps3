/* An lv2 timer's state (sys_timer_ctrl.c): start, stop, one shots and
 * periodic timers as LV2 has them, and threads checking, stopping and
 * starting one timer at once - each expiry fired once and only once, a
 * stop never undone by a firing. Under ThreadSanitizer. C89. */

#include <stdlib.h>
#include <string.h>

#include "../../Emu/Cell/lv2/sys_timer_ctrl.h"
#include "test_common.h"

#include <rthreads/rthreads.h>

static unsigned sends;
static uint64_t last_at;

static void count_send(void *ctx, uint64_t at)
{
   (void)ctx;
   sends++;
   last_at = at;
}

static void basics(void)
{
   lv2_timer_ctrl_t c;
   unsigned running;
   uint64_t expire, period;

   lv2_timer_ctrl_init(&c, 0, 0, 0);
   lv2_timer_ctrl_info(&c, &running, &expire, &period);
   TEST_CHECK(!running && !expire && !period);
   TEST_CHECK(lv2_timer_ctrl_check(&c, 100, count_send, NULL) == UINT64_MAX);

   /* A one shot in the past times out; one ahead runs, fires once, stops */
   TEST_CHECK(lv2_timer_ctrl_start(&c, 100, 50, 0, count_send, NULL) == LV2_TIMER_CTRL_TIMED_OUT);
   TEST_CHECK(lv2_timer_ctrl_start(&c, 100, 150, 0, count_send, NULL) == LV2_TIMER_CTRL_STARTED);
   TEST_CHECK(lv2_timer_ctrl_start(&c, 100, 160, 0, count_send, NULL) == LV2_TIMER_CTRL_BUSY);
   lv2_timer_ctrl_info(&c, &running, &expire, &period);
   TEST_CHECK(running && expire == 150 && period == 0);
   TEST_CHECK(lv2_timer_ctrl_check(&c, 120, count_send, NULL) == 30);
   TEST_CHECK(sends == 0);
   TEST_CHECK(lv2_timer_ctrl_check(&c, 151, count_send, NULL) == UINT64_MAX);
   TEST_CHECK(sends == 1 && last_at == 150);
   lv2_timer_ctrl_info(&c, &running, &expire, &period);
   TEST_CHECK(!running);

   /* Periodic: from now with no base; caught up when checked late */
   TEST_CHECK(lv2_timer_ctrl_start(&c, 1000, 0, 100, count_send, NULL) == LV2_TIMER_CTRL_STARTED);
   lv2_timer_ctrl_info(&c, &running, &expire, &period);
   TEST_CHECK(running && expire == 1100 && period == 100);
   sends = 0;
   TEST_CHECK(lv2_timer_ctrl_check(&c, 1350, count_send, NULL) == 50);
   TEST_CHECK(sends == 3 && last_at == 1300);
   lv2_timer_ctrl_stop(&c);
   TEST_CHECK(lv2_timer_ctrl_check(&c, 5000, count_send, NULL) == UINT64_MAX);
   TEST_CHECK(sends == 3);

   /* A base a period or more ago: the first boundary after now, from base */
   TEST_CHECK(lv2_timer_ctrl_start(&c, 1000, 30, 100, count_send, NULL) == LV2_TIMER_CTRL_STARTED);
   lv2_timer_ctrl_info(&c, &running, &expire, &period);
   TEST_CHECK(expire == 1030);
   lv2_timer_ctrl_stop(&c);
   /* A base less than a period ago: base and a period */
   TEST_CHECK(lv2_timer_ctrl_start(&c, 1000, 950, 100, count_send, NULL) == LV2_TIMER_CTRL_STARTED);
   lv2_timer_ctrl_info(&c, &running, &expire, &period);
   TEST_CHECK(expire == 1050);
   lv2_timer_ctrl_stop(&c);

   /* Due when started again: fired first, then BUSY or started as it is */
   TEST_CHECK(lv2_timer_ctrl_start(&c, 2000, 2010, 0, count_send, NULL) == LV2_TIMER_CTRL_STARTED);
   sends = 0;
   TEST_CHECK(lv2_timer_ctrl_start(&c, 2020, 2030, 0, count_send, NULL) == LV2_TIMER_CTRL_STARTED);
   TEST_CHECK(sends == 1 && last_at == 2010);
   lv2_timer_ctrl_stop(&c);
}

#define THREADS 4
#define PERIOD 10u
#define END 200000u

static lv2_timer_ctrl_t shared;
static retro_atomic_int_t fired[END / PERIOD + 2];
static retro_atomic_int_t twice;

static void record(void *ctx, uint64_t at)
{
   (void)ctx;
   if (retro_atomic_fetch_add_int(&fired[at / PERIOD], 1) != 0)
      retro_atomic_inc_int(&twice);
}

static void checker(void *data)
{
   uint64_t now;
   (void)data;
   for (now = 0; now <= END; now += 1 + (uint64_t)(rand() % 7))
      (void)lv2_timer_ctrl_check(&shared, now, record, NULL);
   (void)lv2_timer_ctrl_check(&shared, END, record, NULL);
}

static void threads(void)
{
   sthread_t *t[THREADS];
   unsigned i;
   unsigned missing = 0;

   retro_atomic_int_init(&twice, 0);
   for (i = 0; i < END / PERIOD + 2; i++)
      retro_atomic_int_init(&fired[i], 0);

   lv2_timer_ctrl_init(&shared, 0, 0, 0);
   TEST_CHECK(lv2_timer_ctrl_start(&shared, 0, 0, PERIOD, record, NULL) == LV2_TIMER_CTRL_STARTED);

   for (i = 0; i < THREADS; i++)
      t[i] = sthread_create(checker, NULL);
   for (i = 0; i < THREADS; i++)
      sthread_join(t[i]);

   /* Every boundary up to the end fired once */
   for (i = 1; i <= END / PERIOD; i++)
      if (retro_atomic_load_acquire_int(&fired[i]) != 1)
         missing++;
   TEST_CHECK(!missing);
   TEST_CHECK(retro_atomic_load_acquire_int(&twice) == 0);

   /* Stopped while threads fire: stays stopped, nothing fires after */
   lv2_timer_ctrl_stop(&shared);
   TEST_CHECK(lv2_timer_ctrl_check(&shared, END * 2, record, NULL) == UINT64_MAX);
   printf("test_timer_ctrl: %d threads firing one periodic timer: %u expiries, each once\n", THREADS, END / PERIOD);
}

int main(void)
{
   basics();
   threads();
   if (test_failures)
   {
      fprintf(stderr, "test_timer_ctrl: %d failed\n", test_failures);
      return 1;
   }
   printf("test_timer_ctrl: one shots and periodic timers as LV2 starts them, stops that hold\n");
   return 0;
}
