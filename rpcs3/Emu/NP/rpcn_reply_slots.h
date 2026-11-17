/* The RPCN client's sync replies, each for the one thread waiting for its
 * id - with no lock.
 *
 * The reader claims a free slot (rpcn_reply_slots_claim), fills what goes
 * with it (kept by the caller, at the same index) and publishes it under
 * the reply's id (rpcn_reply_slots_publish). The thread waiting for that id
 * takes it (rpcn_reply_slots_take), moves what goes with it out, and frees
 * it (rpcn_reply_slots_free). Each step is one compare-and-swap or store on
 * the slot's state; nothing is allocated. C89. */

#ifndef RPCN_REPLY_SLOTS_H
#define RPCN_REPLY_SLOTS_H

#include <stdint.h>
#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RPCN_REPLY_SLOTS 64

typedef struct rpcn_reply_slots
{
   /* Each: 0 free, 1 being filled, 2 full, 3 being taken */
   retro_atomic_int_t state[RPCN_REPLY_SLOTS];
   /* Each full slot's id: read by the waiters as the reader may be
    * publishing another in it, so atomic */
   retro_atomic_64_t  id[RPCN_REPLY_SLOTS];
} rpcn_reply_slots_t;

void rpcn_reply_slots_init(rpcn_reply_slots_t *s);

/* The reader: a free slot, now being filled, or -1 when none is free */
int rpcn_reply_slots_claim(rpcn_reply_slots_t *s);

/* The reader: the slot filled, for the thread waiting for @id */
void rpcn_reply_slots_publish(rpcn_reply_slots_t *s, int slot, uint64_t id);

/* The waiting thread: the full slot for @id, now being taken, or -1 */
int rpcn_reply_slots_take(rpcn_reply_slots_t *s, uint64_t id);

/* The taker (or rpcn_reply_slots_drop's caller): the slot free again */
void rpcn_reply_slots_free(rpcn_reply_slots_t *s, int slot);

/* The next full slot from @from on, now being taken - what no thread waits
 * for any more, as the connection goes - or -1 */
int rpcn_reply_slots_drop(rpcn_reply_slots_t *s, int from);

#ifdef __cplusplus
}
#endif

#endif
