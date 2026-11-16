#pragma once

#include "sys_sync.h"
#include "lv2_lwq.h"

#include "Emu/Memory/vm_ptr.h"

struct sys_lwmutex_attribute_t
{
	be_t<u32> protocol;
	be_t<u32> recursive;

	union
	{
		nse_t<u64, 1> name_u64;
		char name[sizeof(u64)];
	};
};

enum : u32
{
	lwmutex_free     = 0xffffffffu,
	lwmutex_dead     = 0xfffffffeu,
	lwmutex_reserved = 0xfffffffdu,
};

struct sys_lwmutex_t
{
	struct alignas(8) sync_var_t
	{
		be_t<u32> owner;
		be_t<u32> waiter;
	};

	union
	{
		atomic_t<sync_var_t> lock_var;

		struct
		{
			atomic_be_t<u32> owner;
			atomic_be_t<u32> waiter;
		}
		vars;

		atomic_be_t<u64> all_info;
	};

	be_t<u32> attribute;
	be_t<u32> recursive_count;
	be_t<u32> sleep_queue; // lwmutex pseudo-id
	be_t<u32> pad;
};

class ppu_thread;

struct lv2_lwmutex final : lv2_obj
{
	static const u32 id_base = 0x95000000;

	const lv2_protocol protocol;
	const vm::ptr<sys_lwmutex_t> control;
	const be_t<u64> name;

	// The signal and the threads sleeping on it, one word (lv2_lwq.c): no
	// lock, no turn. Each sleeper's thread in its slot, for an unlock to wake
	lv2_lwq_t q;
	retro_atomic_ptr_t thread[LV2_LWQ_SLOTS]{};
	atomic_t<s32> lwcond_waiters{0};

	lv2_lwmutex(u32 protocol, vm::ptr<sys_lwmutex_t> control, u64 name) noexcept
		: protocol{static_cast<u8>(protocol)}
		, control(control)
		, name(std::bit_cast<be_t<u64>>(name))
	{
		lv2_lwq_init(&q, 0);
	}

	lv2_lwmutex(utils::serial& ar);
	void save(utils::serial& ar);

	// A PPU thread's slot: its ID's index
	static u32 slot_of(const ppu_thread& ppu);

	ppu_thread* thread_in(u32 slot)
	{
		return static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(&thread[slot]));
	}

	// A sleeper's rank: its thread's priority, unless the lwmutex is FIFO
	static int rank(void* ctx, unsigned slot);
	lv2_lwq_rank_t ranking() const { return protocol == SYS_SYNC_FIFO ? nullptr : rank; }

	// The signal taken by @cpu, or @cpu put to sleep on the lwmutex - in one
	// change. The signal's bits back when taken; 0 when it sleeps
	s32 try_own(ppu_thread* cpu);

	// Given up as the signal when nobody sleeps
	bool try_unlock(bool unlock2)
	{
		return lv2_lwq_unlock_free(&q, unlock2) != 0;
	}

	// Given up: to the best sleeper, which comes back for the caller to
	// wake; or the signal left (nullptr)
	ppu_thread* reown(bool unlock2 = false)
	{
		const int slot = lv2_lwq_unlock(&q, unlock2, ranking(), this);
		return slot < 0 ? nullptr : ensure(thread_in(static_cast<u32>(slot)));
	}

	// A sleeper that is to be saved again (a save state being made): an
	// unlock is done again after
	bool resaving();
};

// Syscalls

error_code _sys_lwmutex_create(ppu_thread& ppu, vm::ptr<u32> lwmutex_id, u32 protocol, vm::ptr<sys_lwmutex_t> control, s32 has_name, u64 name);
error_code _sys_lwmutex_destroy(ppu_thread& ppu, u32 lwmutex_id);
error_code _sys_lwmutex_lock(ppu_thread& ppu, u32 lwmutex_id, u64 timeout);
error_code _sys_lwmutex_trylock(ppu_thread& ppu, u32 lwmutex_id);
error_code _sys_lwmutex_unlock(ppu_thread& ppu, u32 lwmutex_id);
error_code _sys_lwmutex_unlock2(ppu_thread& ppu, u32 lwmutex_id);
