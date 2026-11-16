#pragma once

#include "sys_sync.h"
#include "lv2_rwq.h"

#include "Emu/Memory/vm_ptr.h"

struct sys_rwlock_attribute_t
{
	be_t<u32> protocol;
	be_t<u32> pshared;
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

struct lv2_rwlock final : lv2_obj
{
	static const u32 id_base = 0x88000000;

	const lv2_protocol protocol;
	const u64 key;
	const u64 name;

	// The owners and the waiters, with no lock (lv2_rwq.c); each waiter's
	// thread in its slot, for an unlock to wake, and the writer's ID (for a
	// save state)
	lv2_rwq_t q;
	retro_atomic_ptr_t thread[LV2_RWQ_SLOTS]{};
	retro_atomic_int_t writer_id{};

	lv2_rwlock(u32 protocol, u64 key, u64 name) noexcept
		: protocol{static_cast<u8>(protocol)}
		, key(key)
		, name(name)
	{
		lv2_rwq_init(&q, -1, 0);
	}

	lv2_rwlock(utils::serial& ar);
	static std::function<void(void*)> load(utils::serial& ar);
	void save(utils::serial& ar);

	// A PPU thread's slot: its ID's index
	static u32 slot_of_id(u32 id);
};

// Syscalls

error_code sys_rwlock_create(ppu_thread& ppu, vm::ptr<u32> rw_lock_id, vm::ptr<sys_rwlock_attribute_t> attr);
error_code sys_rwlock_destroy(ppu_thread& ppu, u32 rw_lock_id);
error_code sys_rwlock_rlock(ppu_thread& ppu, u32 rw_lock_id, u64 timeout);
error_code sys_rwlock_tryrlock(ppu_thread& ppu, u32 rw_lock_id);
error_code sys_rwlock_runlock(ppu_thread& ppu, u32 rw_lock_id);
error_code sys_rwlock_wlock(ppu_thread& ppu, u32 rw_lock_id, u64 timeout);
error_code sys_rwlock_trywlock(ppu_thread& ppu, u32 rw_lock_id);
error_code sys_rwlock_wunlock(ppu_thread& ppu, u32 rw_lock_id);

constexpr auto _sys_rwlock_trywlock = sys_rwlock_trywlock;
