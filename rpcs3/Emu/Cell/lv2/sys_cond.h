#pragma once

#include "sys_sync.h"
#include "sys_mutex.h"
#include "lv2_waitset.h"

struct lv2_mutex;

struct sys_cond_attribute_t
{
	be_t<u32> pshared;
	be_t<s32> flags;
	be_t<u64> ipc_key;

	union
	{
		nse_t<u64, 1> name_u64;
		char name[sizeof(u64)];
	};
};

struct lv2_cond final : lv2_obj
{
	static const u32 id_base = 0x86000000;

	const u64 key;
	const u64 name;
	const u32 mtx_id;

	lv2_mutex* mutex; // Associated Mutex
	shared_ptr<lv2_obj> _mutex;

	// The threads waiting on it, a bit each with the order they came in
	// (lv2_waitset.h): joined, left on a timeout and woken with one
	// compare-and-swap each. Woken, a thread is handed to the mutex in one
	// more (lv2_ownq_give). No lock, no turn
	lv2_waitset_t waiters;
	retro_atomic_ptr_t thread[LV2_OWNQ_SLOTS]{};

	ppu_thread* thread_in(u32 slot)
	{
		return static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(&thread[slot]));
	}

	// Whether a thread waiting on it is to be saved again (a save state is
	// being made): nothing is done to the waiters then
	bool any_waiter_again();

	lv2_cond(u64 key, u64 name, u32 mtx_id, shared_ptr<lv2_obj> mutex0) noexcept;

	lv2_cond(utils::serial& ar) noexcept;
	static std::function<void(void*)> load(utils::serial& ar);
	void save(utils::serial& ar);

	CellError on_id_create();
};

class ppu_thread;

// Syscalls

error_code sys_cond_create(ppu_thread& ppu, vm::ptr<u32> cond_id, u32 mutex_id, vm::ptr<sys_cond_attribute_t> attr);
error_code sys_cond_destroy(ppu_thread& ppu, u32 cond_id);
error_code sys_cond_wait(ppu_thread& ppu, u32 cond_id, u64 timeout);
error_code sys_cond_signal(ppu_thread& ppu, u32 cond_id);
error_code sys_cond_signal_all(ppu_thread& ppu, u32 cond_id);
error_code sys_cond_signal_to(ppu_thread& ppu, u32 cond_id, u32 thread_id);
