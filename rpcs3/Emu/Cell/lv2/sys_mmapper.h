#pragma once

#include <retro_atomic.h>

#include "sys_sync.h"

#include "Emu/Memory/vm_ptr.h"
#include "Emu/Cell/ErrorCodes.h"

#include "util/shared_ptr.hpp"

#include <vector>

struct lv2_memory_container;

namespace utils
{
	class shm;
}

struct lv2_memory : lv2_obj
{
	static const u32 id_base = 0x08000000;

	const u32 size; // Memory size
	const u32 align; // Alignment required
	const u64 flags;
	const u64 key; // IPC key
	const bool pshared; // Process shared flag
	lv2_memory_container* const ct; // null for system memory
	u32 system_handle = 0; // stands in for the vsh handle
	atomic_ptr<std::shared_ptr<utils::shm>> shm;

	retro_atomic_int_t counter{}; // a u32
	// the game can drop its handle while audio still needs the buffer
	retro_atomic_int_t external_refs{}; // a u32
	// Its container's memory taken for it (1) or given back (0): taken and
	// given back once each, by whichever sees it come and go first - the
	// game's free and audio's last reference meet with no lock
	retro_atomic_int_t holds{};

	lv2_memory(u32 size, u32 align, u64 flags, u64 key, bool pshared, lv2_memory_container* ct);

	lv2_memory(utils::serial& ar);
	static std::function<void(void*)> load(utils::serial& ar);
	void save(utils::serial& ar);
	void save_data(utils::serial& ar);
	void release_memory();

	CellError on_id_create();
	CellError on_id_create(u64 requested_size, u32 requested_align, const shared_ptr<lv2_memory>& created);
};

enum : u64
{
	SYS_MMAPPER_SYSUTIL_SHM_KEY = 0x8006010000000010ULL,
	SYS_MMAPPER_MIO_SHM_KEY = 0x80004d494f323211ULL,
	SYS_MEMORY_PAGE_FAULT_EVENT_KEY	       = 0xfffe000000000000ULL,
};

enum : u64
{
	SYS_MMAPPER_NO_SHM_KEY = 0xffff000000000000ull, // Unofficial name
};

enum : u64
{
	SYS_MEMORY_PAGE_FAULT_CAUSE_NON_MAPPED = 0x2ULL,
	SYS_MEMORY_PAGE_FAULT_CAUSE_READ_ONLY  = 0x1ULL,
	SYS_MEMORY_PAGE_FAULT_TYPE_PPU_THREAD  = 0x0ULL,
	SYS_MEMORY_PAGE_FAULT_TYPE_SPU_THREAD  = 0x1ULL,
	SYS_MEMORY_PAGE_FAULT_TYPE_RAW_SPU     = 0x2ULL,
};

struct page_fault_notification_entry
{
	ENABLE_BITWISE_SERIALIZATION;

	u32 start_addr; // Starting address of region to monitor.
	u32 event_queue_id; // Queue to be notified.
	u32 port_id; // Port used to notify the queue.
};

// The queues to be told of a page fault, no lock: one slot per 256 MB, as
// the areas watched (sys_mmapper_allocate_address) start on such a bound
// and are that big at least. A slot holds the queue's ID in the high half
// and the port's in the low (never 0), 0 for none; set and cleared by
// compare-and-swap
struct page_fault_notification_entries
{
	retro_atomic_64_t slots[16]{};

	static u32 slot_of(u32 start_addr) { return start_addr >> 28; }

	// The port of the area starting at @start_addr, 0 for none
	u32 port_of(u32 start_addr)
	{
		return (start_addr & 0x0fffffff) ? 0 : static_cast<u32>(retro_atomic_load_acquire_64(&slots[slot_of(start_addr)]));
	}

	SAVESTATE_INIT_POS(44);

	page_fault_notification_entries() = default;
	page_fault_notification_entries(utils::serial& ar);
	void save(utils::serial& ar);
};

// A thread waiting out a page fault is marked in its own word
// (cpu_thread::pf_fault); this counts them, so that freeing an area looks
// at the threads only when there are any
struct page_fault_event_entries
{
	retro_atomic_int_t faulted{0};
};

struct mmapper_unk_entry_struct0
{
	be_t<u32> a;    // 0x0
	be_t<u32> b;    // 0x4
	be_t<u32> c;    // 0x8
	be_t<u32> d;    // 0xc
	be_t<u64> type; // 0x10
};

// Aux
class ppu_thread;

void init_system_shared_memory();
error_code mmapper_thread_recover_page_fault(cpu_thread* cpu);

// SysCalls
error_code sys_mmapper_allocate_address(ppu_thread&, u64 size, u64 flags, u64 alignment, vm::ptr<u32> alloc_addr);
error_code sys_mmapper_allocate_fixed_address(ppu_thread&);
error_code sys_mmapper_allocate_shared_memory(ppu_thread&, u64 ipc_key, u64 size, u64 flags, vm::ptr<u32> mem_id);
error_code sys_mmapper_allocate_shared_memory_from_container(ppu_thread&, u64 ipc_key, u64 size, u32 cid, u64 flags, vm::ptr<u32> mem_id);
error_code sys_mmapper_allocate_shared_memory_ext(ppu_thread&, u64 ipc_key, u64 size, u32 flags, vm::ptr<mmapper_unk_entry_struct0> entries, s32 entry_count, vm::ptr<u32> mem_id);
error_code sys_mmapper_allocate_shared_memory_from_container_ext(ppu_thread&, u64 ipc_key, u64 size, u64 flags, u32 cid, vm::ptr<mmapper_unk_entry_struct0> entries, s32 entry_count, vm::ptr<u32> mem_id);
error_code sys_mmapper_change_address_access_right(ppu_thread&, u32 addr, u64 flags);
error_code sys_mmapper_free_address(ppu_thread&, u32 addr);
error_code sys_mmapper_free_shared_memory(ppu_thread&, u32 mem_id);
error_code sys_mmapper_map_shared_memory(ppu_thread&, u32 addr, u32 mem_id, u64 flags);
error_code sys_mmapper_search_and_map(ppu_thread&, u32 start_addr, u32 mem_id, u64 flags, vm::ptr<u32> alloc_addr);
error_code sys_mmapper_unmap_shared_memory(ppu_thread&, u32 addr, vm::ptr<u32> mem_id);
error_code sys_mmapper_enable_page_fault_notification(ppu_thread&, u32 start_addr, u32 event_queue_id);
