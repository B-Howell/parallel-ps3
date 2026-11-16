#pragma once

#include "sys_sync.h"
#include "lv2_flagq.h"

#include "Emu/Memory/vm_ptr.h"

enum
{
	SYS_SYNC_WAITER_SINGLE = 0x10000,
	SYS_SYNC_WAITER_MULTIPLE = 0x20000,

	SYS_EVENT_FLAG_WAIT_AND = 0x01,
	SYS_EVENT_FLAG_WAIT_OR = 0x02,

	SYS_EVENT_FLAG_WAIT_CLEAR = 0x10,
	SYS_EVENT_FLAG_WAIT_CLEAR_ALL = 0x20,
};

struct sys_event_flag_attribute_t
{
	be_t<u32> protocol;
	be_t<u32> pshared;
	be_t<u64> ipc_key;
	be_t<s32> flags;
	be_t<s32> type;

	union
	{
		nse_t<u64, 1> name_u64;
		char name[sizeof(u64)];
	};
};

class ppu_thread;

struct lv2_event_flag final : lv2_obj
{
	static const u32 id_base = 0x98000000;

	const lv2_protocol protocol;
	const u64 key;
	const s32 type;
	const u64 name;

	// The bits and the waiters, with no lock (lv2_flagq.c); each waiter's
	// thread in its slot, for a set to wake
	lv2_flagq_t q;
	retro_atomic_ptr_t thread[LV2_FLAGQ_SLOTS]{};

	lv2_event_flag(u32 protocol, u64 key, s32 type, u64 name, u64 pattern) noexcept
		: protocol{static_cast<u8>(protocol)}
		, key(key)
		, type(type)
		, name(name)
	{
		lv2_flagq_init(&q, pattern);
	}

	lv2_event_flag(utils::serial& ar);
	static std::function<void(void*)> load(utils::serial& ar);
	void save(utils::serial& ar);

	// A PPU thread's slot: its ID's index
	static u32 slot_of(const ppu_thread& ppu);

	// Check mode arg
	static bool check_mode(u32 mode)
	{
		return lv2_flagq_mode_ok(mode) != 0;
	}
};

// Syscalls

error_code sys_event_flag_create(ppu_thread& ppu, vm::ptr<u32> id, vm::ptr<sys_event_flag_attribute_t> attr, u64 init);
error_code sys_event_flag_destroy(ppu_thread& ppu, u32 id);
error_code sys_event_flag_wait(ppu_thread& ppu, u32 id, u64 bitptn, u32 mode, vm::ptr<u64> result, u64 timeout);
error_code sys_event_flag_trywait(ppu_thread& ppu, u32 id, u64 bitptn, u32 mode, vm::ptr<u64> result);
error_code sys_event_flag_set(cpu_thread& cpu, u32 id, u64 bitptn);
error_code sys_event_flag_clear(ppu_thread& ppu, u32 id, u64 bitptn);
error_code sys_event_flag_cancel(ppu_thread& ppu, u32 id, vm::ptr<u32> num);
error_code sys_event_flag_get(ppu_thread& ppu, u32 id, vm::ptr<u64> flags);
