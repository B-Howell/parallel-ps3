#pragma once

#include "sys_sync.h"
#include "lv2_ownq.h"

#include "Emu/Memory/vm_ptr.h"

#include "Emu/Cell/PPUThread.h"

struct sys_mutex_attribute_t
{
	be_t<u32> protocol; // SYS_SYNC_FIFO, SYS_SYNC_PRIORITY or SYS_SYNC_PRIORITY_INHERIT
	be_t<u32> recursive; // SYS_SYNC_RECURSIVE or SYS_SYNC_NOT_RECURSIVE
	be_t<u32> pshared;
	be_t<u32> adaptive;
	be_t<u64> ipc_key;
	be_t<s32> flags;
	be_t<u32> pad;

	union
	{
		nse_t<u64, 1> name_u64;
		char name[sizeof(u64)];
	};
};

class ppu_thread;

struct lv2_mutex final : lv2_obj
{
	static const u32 id_base = 0x85000000;

	const lv2_protocol protocol;
	const u32 recursive;
	const u32 adaptive;
	const u64 key;
	const u64 name;

	retro_atomic_int_t cond_count{}; // Condition Variables
	retro_atomic_int_t lock_count{}; // Recursive Locks (a u32): the owner's alone

	// The owner and the threads waiting for it, in one 128-bit word
	// (lv2_ownq.h): taking it, queueing, leaving the queue on a timeout,
	// handing it to the best waiter at unlock and to a thread a condition
	// wakes are each one compare-and-swap. No lock, no turn
	lv2_ownq_t q;

	// The thread in each slot while it waits or owns, for an unlock to wake
	// it and a priority mutex to rank it
	retro_atomic_ptr_t thread[LV2_OWNQ_SLOTS]{};

	// The owner's ID, for a save state
	retro_atomic_int_t owner_id{};

	lv2_mutex(u32 protocol, u32 recursive,u32 adaptive, u64 key, u64 name) noexcept
		: protocol{static_cast<u8>(protocol)}
		, recursive(recursive)
		, adaptive(adaptive)
		, key(key)
		, name(name)
	{
		lv2_ownq_init(&q, -1);
	}

	lv2_mutex(utils::serial& ar);
	static std::function<void(void*)> load(utils::serial& ar);
	void save(utils::serial& ar);

	// A PPU thread's slot: its ID's index
	static u32 slot_of_id(u32 id);

	static u32 slot_of(const ppu_thread& ppu)
	{
		return slot_of_id(ppu.id);
	}

	bool owned_by(const ppu_thread& ppu)
	{
		return lv2_ownq_owner(&q) == static_cast<int>(slot_of(ppu));
	}

	ppu_thread* thread_in(u32 slot)
	{
		return static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(&thread[slot]));
	}

	// The rank lv2_ownq_unlock goes by: the waiter's priority for a priority
	// mutex, none (the order they came) for a FIFO one
	static int rank(void* ctx, unsigned slot);

	lv2_ownq_rank_t ranking()
	{
		return protocol == SYS_SYNC_FIFO ? nullptr : rank;
	}

	// The thread in @slot made the owner: noted, for a save state
	void now_owned_by(u32 slot)
	{
		if (const auto ppu = thread_in(slot))
		{
			retro_atomic_store_release_int(&owner_id, static_cast<int>(ppu->id));
		}
	}

	template <typename T>
	CellError try_lock(T& ppu)
	{
		const u32 slot = slot_of_id(ppu.id);

		if constexpr (std::is_same_v<T, ppu_thread>)
		{
			retro_atomic_store_release_ptr(&thread[slot], &ppu);
		}

		switch (lv2_ownq_trylock(&q, slot))
		{
		case LV2_OWNQ_OWNED:
			retro_atomic_store_release_int(&owner_id, static_cast<int>(ppu.id));
			return {};
		case LV2_OWNQ_MINE:
			// Recursive locking
			if (recursive == SYS_SYNC_RECURSIVE)
			{
				if (static_cast<u32>(retro_atomic_load_acquire_int(&lock_count)) == 0xffffffffu)
				{
					return CELL_EKRESOURCE;
				}

				retro_atomic_fetch_add_int(&lock_count, 1);
				return {};
			}

			return CELL_EDEADLK;
		default:
			return CELL_EBUSY;
		}
	}

	// The owner now (true), or queued in the same change (false)
	bool try_own(ppu_thread& ppu)
	{
		const u32 slot = slot_of(ppu);
		retro_atomic_store_release_ptr(&thread[slot], &ppu);

		if (lv2_ownq_give(&q, slot) == LV2_OWNQ_OWNED)
		{
			retro_atomic_store_release_int(&owner_id, static_cast<int>(ppu.id));
			return true;
		}

		return false;
	}
};


// Syscalls

error_code sys_mutex_create(ppu_thread& ppu, vm::ptr<u32> mutex_id, vm::ptr<sys_mutex_attribute_t> attr);
error_code sys_mutex_destroy(ppu_thread& ppu, u32 mutex_id);
error_code sys_mutex_lock(ppu_thread& ppu, u32 mutex_id, u64 timeout);
error_code sys_mutex_trylock(ppu_thread& ppu, u32 mutex_id);
error_code sys_mutex_unlock(ppu_thread& ppu, u32 mutex_id);
