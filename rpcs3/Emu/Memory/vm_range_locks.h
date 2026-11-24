#ifndef VM_RANGE_LOCKS_H__
#define VM_RANGE_LOCKS_H__

/* The memory range locks (vm_locking.h): 64 slots, each a word on a cache
 * line of its own holding the range a thread is accessing (address in the
 * low half, size and flags in the high), and two words of bits, one for
 * the slots in use and one for the slots locked for writing. Slots are
 * taken and given back with compare-and-swap; the scans below look at the
 * slots named by a mask and give back the ones that overlap. Lock-free. */

#include <stdint.h>
#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VM_RANGE_LOCKS 64

/* In the high half: the size (29 bits) and three flag bits */
#define VM_RANGE_FLAG_BITS 3
#define VM_RANGE_FULL_MASK_HI  (7u << 29)
#define VM_RANGE_LOCKED_HI     (4u << 29)
#define VM_RANGE_READABLE_HI   (2u << 29)

typedef struct vm_range_lock_slot
{
   retro_atomic_64_t word;
   unsigned char pad[128 - 8];
} vm_range_lock_slot_t;

/* A slot's range set before the writers' bits are looked at, or a
 * writer's bit set before the slots are (two sides that must each see the
 * other): on x86 the exchange is itself the full barrier, the one
 * instruction a plain store would need a fence after; elsewhere a release
 * store and a full fence */
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define VM_RANGE_RMW_IS_FENCE 1
#endif

static INLINE void vm_range_lock_publish(retro_atomic_64_t *word, uint64_t value)
{
#ifdef VM_RANGE_RMW_IS_FENCE
   (void)retro_atomic_exchange_64(word, (int64_t)value);
#else
   retro_atomic_store_release_64(word, (int64_t)value);
   retro_atomic_thread_fence_seq_cst();
#endif
}

/* After a read-modify-write of the bits, before the slots are looked at */
static INLINE void vm_range_lock_fence_after_rmw(void)
{
#ifndef VM_RANGE_RMW_IS_FENCE
   retro_atomic_thread_fence_seq_cst();
#endif
}

/* Takes a free slot from @bits (its top bit is kept for memory setting
 * changes): the slot, or -1 when none is free */
int vm_range_lock_alloc(retro_atomic_64_t *bits);

/* Gives @slot back: its word cleared, then its bit */
void vm_range_lock_free(retro_atomic_64_t *bits, vm_range_lock_slot_t *set, unsigned slot);

/* Of the slots in @mask, those locked over [@begin, @begin + @size): a
 * reader's look before it goes on (range_lock_internal). A slot locked
 * whole (VM_RANGE_LOCKED) covers 128 bytes; @is_share is the shared
 * memory base of @begin's page, @shmem the table by 64K page */
uint64_t vm_range_locks_busy_reader(uint64_t mask, vm_range_lock_slot_t *set,
      retro_atomic_64_t *shmem, uint32_t begin, uint32_t size, uint64_t is_share);

/* Of the slots in @mask, those whose range overlaps [@addr, @addr + @size)
 * as plain 32-bit ranges (a memory setting change waiting for readers) */
uint64_t vm_range_locks_busy_overlap(uint64_t mask, vm_range_lock_slot_t *set,
      uint32_t addr, uint32_t size);

/* Of the slots in @mask, those whose range, page by page through the
 * shared memory table, holds the 128-byte line @point (a writer waiting
 * for readers of one reservation); readable-only slots never do */
uint64_t vm_range_locks_busy_point(uint64_t mask, vm_range_lock_slot_t *set,
      retro_atomic_64_t *shmem, uint64_t point);

/* Waiting for the writers' bits to change, with no spin or yield: a
 * waiter counts itself in @waiters, takes @epoch, looks again, and waits
 * on @epoch (and whatever else it waits for); a writer that has cleared
 * its bit bumps @epoch and wakes it, only when someone waits. On a line of
 * its own, read-mostly: a release with no waiter pays one load of it */
typedef struct vm_writer_wake
{
   retro_atomic_int_t waiters;
   retro_atomic_int_t epoch;
   unsigned char pad[128 - 2 * sizeof(retro_atomic_int_t)];
} vm_writer_wake_t;

/* A waiter: counted, and the epoch to wait on from here (look at the bits
 * after this, then wait if they have not changed) */
uint32_t vm_writer_wait_begin(vm_writer_wake_t *wake);

/* A waiter no more */
void vm_writer_wait_end(vm_writer_wake_t *wake);

/* After a writer clears its bit: wakes the waiters, if there are any */
void vm_writer_released(vm_writer_wake_t *wake);

#ifdef __cplusplus
}
#endif

#endif
