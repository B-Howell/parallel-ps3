#pragma once

#include "Emu/Memory/guest_atomic.h"

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

	// The owner (the first word) and the count of waiters, in the game's
	// memory, which its own code changes too (lwarx/stwcx): changed from
	// the host through guest_atomic.h, the pair as one 64-bit word
	union
	{
		sync_var_t vars;

		be_t<u64> all_info;
	};

	u32 owner() noexcept
	{
		return guest_atomic_load_be32(&vars.owner);
	}

	void set_owner(u32 owner) noexcept
	{
		guest_atomic_store_be32(&vars.owner, owner);
	}

	// The owner before
	u32 swap_owner(u32 owner) noexcept
	{
		return guest_atomic_exchange_be32(&vars.owner, owner);
	}

	// The owner before; @owner put in where it was @cmp
	u32 cas_owner(u32 cmp, u32 owner) noexcept
	{
		while (true)
		{
			const u32 old = this->owner();

			if (old != cmp || guest_atomic_cas_be32(&vars.owner, cmp, owner))
			{
				return old;
			}
		}
	}

	bool cas_owner_test(u32 cmp, u32 owner) noexcept
	{
		return guest_atomic_cas_be32(&vars.owner, cmp, owner);
	}

	// Waiters added (or taken, @n negative): the low word of the pair
	void add_waiters(s64 n) noexcept
	{
		guest_atomic_fetch_add_be64(&all_info, static_cast<u64>(n));
	}

	void set_lock(u32 owner, u32 waiter) noexcept
	{
		guest_atomic_store_be64(&all_info, u64{owner} << 32 | waiter);
	}

	bool cas_lock_test(u32 cmp_owner, u32 cmp_waiter, u32 owner, u32 waiter) noexcept
	{
		return guest_atomic_cas_be64(&all_info, u64{cmp_owner} << 32 | cmp_waiter, u64{owner} << 32 | waiter);
	}

	// The owner set and the waiters moved by @waiters, as one
	void set_owner_add_waiters(u32 owner, s32 waiters) noexcept
	{
		while (true)
		{
			const u64 old = guest_atomic_load_be64(&all_info);
			const u64 next = u64{owner} << 32 | static_cast<u32>(static_cast<u32>(old) + static_cast<u32>(waiters));

			if (guest_atomic_cas_be64(&all_info, old, next))
			{
				return;
			}
		}
	}

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
	retro_atomic_int_t lwcond_waiters{0};

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
