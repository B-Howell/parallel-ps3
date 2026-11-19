#pragma once

#include "vm.h"
#include "Emu/RSX/Utils/rsx_utils.h"
#include "vm_range_locks.h"

class cpu_thread;
class shared_mutex;

namespace vm
{
	extern thread_local atomic_t<cpu_thread*>* g_tls_locked;

	enum range_lock_flags : u64
	{
		/* flags (3 bits, W + R + Reserved) */

		range_writable = 4ull << 61,
		range_readable = 2ull << 61,
		range_reserved = 1ull << 61,
		range_full_mask = 7ull << 61,

		/* flag combinations with special meaning */

		range_locked = 4ull << 61, // R+W as well, but being exclusively accessed (size extends addr)
		range_allocation = 0, // Allocation, no safe access, g_shmem may change at ANY location

		range_pos = 61,
		range_bits = 3,
	};

	// A range lock: one slot (vm_range_locks.h), on a cache line of its own
	using range_lock_t = vm_range_lock_slot_t;

	static_assert(sizeof(range_lock_t) == 128);
	static_assert(range_locked >> 32 == VM_RANGE_LOCKED_HI && range_readable >> 32 == VM_RANGE_READABLE_HI && range_full_mask >> 32 == VM_RANGE_FULL_MASK_HI && range_bits == VM_RANGE_FLAG_BITS);

	// Slot bits: [0] the slots in use, [1] the slots locked for writing
	// (all ones: every slot, a memory setting change)
	extern range_lock_t g_range_lock_bits[2];

	// The slots; the last is the memory setting change's own
	extern range_lock_t g_range_lock_set[VM_RANGE_LOCKS];

	inline u64 range_lock_value(range_lock_t* lock) { return static_cast<u64>(retro_atomic_load_acquire_64(&lock->word)); }

	// Set before the writers' bits are looked at (see vm_range_lock_publish)
	inline void range_lock_publish(range_lock_t* lock, u64 value) { vm_range_lock_publish(&lock->word, value); }

	inline void range_lock_release(range_lock_t* lock, u64 value = 0) { retro_atomic_store_release_64(&lock->word, static_cast<s64>(value)); }

	inline u64 range_lock_writers() { return range_lock_value(&g_range_lock_bits[1]); }

	// The memory setting change's own slot
	inline u64 range_lock_main() { return range_lock_value(&g_range_lock_set[VM_RANGE_LOCKS - 1]); }

	// Per 64K page: the shared memory mirror's base (high bits) or 0
	extern retro_atomic_64_t g_shmem[];

	inline u64 shmem_of(u32 page64k) { return static_cast<u64>(retro_atomic_load_acquire_64(&g_shmem[page64k])); }

	// Register reader
	void passive_lock(cpu_thread& cpu);

	// Register range lock for further use
	range_lock_t* alloc_range_lock();

	void range_lock_internal(range_lock_t* range_lock, u32 begin, u32 size);

	// Lock memory range ignoring memory protection (Size!=0 also implies aligned begin)
	template <uint Size = 0>
	FORCE_INLINE void range_lock(range_lock_t* range_lock, u32 begin, u32 _size)
	{
		if constexpr (Size == 0)
		{
			if (begin >> 28 == rsx::constants::local_mem_base >> 28)
			{
				return;
			}
		}

		// Optimistic locking.
		// Note that we store the range we will be accessing, without any clamping.
		range_lock_publish(range_lock, begin | (u64{_size} << 32));

		// Old-style conditional constexpr
		const u32 size = Size ? Size : _size;

		if (Size == 1 || (begin % 4096 + size % 4096) / 4096 == 0 ? !vm::check_addr(begin) : !vm::check_addr(begin, vm::page_readable, size))
		{
			range_lock_release(range_lock);
			range_lock_internal(range_lock, begin, _size);
			return;
		}

		#ifndef _MSC_VER
		__asm__(""); // Tiny barrier
		#endif

		if (!range_lock_writers()) [[likely]]
		{
			return;
		}

		// Fallback to slow path
		range_lock_internal(range_lock, begin, size);
	}

	// Release it
	void free_range_lock(range_lock_t*) noexcept;

	// Optimization (set cpu_flag::memory)
	bool temporary_unlock(cpu_thread& cpu) noexcept;
	void temporary_unlock() noexcept;

	struct writer_lock final
	{
		range_lock_t* range_lock;

		writer_lock(const writer_lock&) = delete;
		writer_lock& operator=(const writer_lock&) = delete;
		writer_lock() noexcept;
		writer_lock(u32 addr, range_lock_t* range_lock = nullptr, u32 size = 128, u64 flags = range_locked) noexcept;
		~writer_lock() noexcept;
	};
} // namespace vm
