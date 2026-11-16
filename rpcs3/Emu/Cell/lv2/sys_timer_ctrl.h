/* An lv2 timer's state - stopped or running, when it next expires and its
 * period - changed by the guest's syscalls and fired by the timer thread,
 * with no lock and no thread waiting for another. C89.
 *
 * The expiry and the state are one 128-bit word, changed with one
 * compare-and-swap: the low half the expiry, the high half the state in
 * its two low bits and a count of changes above them, so a change made
 * from a state that has moved on fails and is made again from the new
 * one. The period is a word of its own, written only while the timer is
 * being set up (SETUP), which no other change makes use of.
 *
 * Firing is claimed like any other change (RUN to FIRING): whoever claims
 * it - the timer thread, or a syscall that finds the timer due - sends the
 * event, once, and then makes the timer run again a period later, or stop.
 * A stop meanwhile wins: the firer's last change then fails and leaves the
 * timer stopped. Every operation is a few compare-and-swaps, retried only
 * when another thread's change got in first. */

#ifndef LV2_TIMER_CTRL_H
#define LV2_TIMER_CTRL_H

#include <stdint.h>

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV2_TIMER_CTRL_STOP   0u
#define LV2_TIMER_CTRL_RUN    1u
#define LV2_TIMER_CTRL_SETUP  2u
#define LV2_TIMER_CTRL_FIRING 3u

typedef struct lv2_timer_ctrl
{
   retro_atomic_128_t word;   /* lo: expiry; hi: changes << 2 | state */
   retro_atomic_64_t  period; /* 0: one shot */
} lv2_timer_ctrl_t;

/* The event, for the one that fired the timer: at its expiry @at */
typedef void (*lv2_timer_ctrl_send_t)(void *ctx, uint64_t at);

void lv2_timer_ctrl_init(lv2_timer_ctrl_t *c, unsigned running,
      uint64_t expire, uint64_t period);

/* Fires the timer if it is running and due at @now, as often as it is due.
 * The time until it is due next, or UINT64_MAX when it is not running */
uint64_t lv2_timer_ctrl_check(lv2_timer_ctrl_t *c, uint64_t now,
      lv2_timer_ctrl_send_t send, void *ctx);

#define LV2_TIMER_CTRL_STARTED    0
#define LV2_TIMER_CTRL_BUSY       1  /* running already: CELL_EBUSY */
#define LV2_TIMER_CTRL_TIMED_OUT  2  /* a one shot already past: CELL_ETIMEDOUT */

/* sys_timer_start's: fires it first if it is due (at @now), then starts it
 * if stopped. @period 0 is a one shot at @base, else periodic from @base
 * (or @now with @base 0) */
int lv2_timer_ctrl_start(lv2_timer_ctrl_t *c, uint64_t now, uint64_t base,
      uint64_t period, lv2_timer_ctrl_send_t send, void *ctx);

/* Stops it, whatever it is doing */
void lv2_timer_ctrl_stop(lv2_timer_ctrl_t *c);

/* For sys_timer_get_information and a save state: running (a timer being
 * fired is), its expiry and period; stopped, 0 and 0 */
void lv2_timer_ctrl_info(lv2_timer_ctrl_t *c, unsigned *running,
      uint64_t *expire, uint64_t *period);

#ifdef __cplusplus
}
#endif

#endif
