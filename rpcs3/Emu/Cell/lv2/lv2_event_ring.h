/* An lv2 event queue's events, oldest first: sent and received from any
 * thread with no lock. C89.
 *
 * A bounded ring of cells, each with a sequence number (Vyukov's): a
 * sender claims the next cell to write with one compare-and-swap on the
 * tail, writes the event and publishes it with the cell's sequence; a
 * receiver claims the next to read on the head the same way. The queue's
 * own size (127 at most) is kept with a count taken before the cell, so a
 * full queue answers busy exactly at its size. Made with the queue; sending
 * and receiving allocate nothing.
 *
 * An event sent is seen by a receive once its sender has published it: a
 * receive that comes between a sender's claim and its publish finds the
 * queue empty of it, as if the send came a moment later. */

#ifndef LV2_EVENT_RING_H
#define LV2_EVENT_RING_H

#include <stddef.h>
#include <stdint.h>

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lv2_event_cell
{
   retro_atomic_64_t seq;
   uint64_t          data[4];
} lv2_event_cell_t;

typedef struct lv2_event_ring
{
   lv2_event_cell_t  *cells;
   uint64_t           mask;      /* cells less one, a power of two less one */
   int                size;      /* events it holds at most */
   retro_atomic_int_t count;     /* events claimed room for */
   retro_atomic_64_t  head;
   retro_atomic_64_t  tail;
} lv2_event_ring_t;

/* Room for @size events. 0 when the cells cannot be had */
int lv2_event_ring_init(lv2_event_ring_t *r, int size);
void lv2_event_ring_deinit(lv2_event_ring_t *r);

/* 0 when the queue holds its size already */
int lv2_event_ring_push(lv2_event_ring_t *r, const uint64_t ev[4]);

/* The oldest event published into @ev: 0 when there is none */
int lv2_event_ring_pop(lv2_event_ring_t *r, uint64_t ev[4]);

/* Events held: as of the load */
int lv2_event_ring_count(lv2_event_ring_t *r);

/* The events in order without taking them, for a save state or a log,
 * with nothing else using the ring: @fn for each */
typedef void (*lv2_event_ring_each_t)(void *ctx, const uint64_t ev[4]);
void lv2_event_ring_each(lv2_event_ring_t *r, lv2_event_ring_each_t fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
