#ifndef SPU_PC_TABLE_H__
#define SPU_PC_TABLE_H__

/* How many SPUs are running the MFC command at each local storage
 * address at once ("Preferred SPU Threads"): a count a byte for each of
 * the 65536 instruction slots, four to a 32-bit word. The low 7 bits of
 * a byte are the count, the top one is set while some SPU waits for the
 * count to come down, so a leave that finds it clear costs one atomic
 * subtract and no wake. Lock-free; the wait sleeps on the word. */

#include <stdint.h>
#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPU_PC_SLOTS 65536u

typedef struct spu_pc_table
{
   retro_atomic_int_t words[SPU_PC_SLOTS / 4];
} spu_pc_table_t;

/* The count at @slot (pc / 4) */
unsigned spu_pc_count(spu_pc_table_t *table, uint32_t slot);

void spu_pc_enter(spu_pc_table_t *table, uint32_t slot);

/* One out: wakes the SPUs waiting on the slot's word if there are any */
void spu_pc_leave(spu_pc_table_t *table, uint32_t slot);

/* Waits while the count at @slot is @max or more, each sleep bounded by
 * @timeout_us: 1 once it is below, 0 when a sleep ran its bound out */
int spu_pc_wait_below(spu_pc_table_t *table, uint32_t slot, unsigned max,
      int64_t timeout_us);

#ifdef __cplusplus
}
#endif

#endif
