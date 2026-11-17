#include "stdafx.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/lv2/sys_mmapper.h"
#include "Emu/savestate_utils.hpp"
#include "sysPrxForUser.h"

LOG_CHANNEL(sysPrxForUser);

u32 ppu_get_exported_func_addr(u32 fnid, const std::string& module_name);

namespace
{
	// Made once, by the first caller to publish it (compare-and-swap); the
	// HLE's own word, not the game's
	vm::gvar<u32> s_shared_memory_area;
}

error_code sys_mmapper_get_shared_memory_area(ppu_thread& ppu, u64 flags, vm::ptr<u32> address)
{
	if (const u32 entry = ppu_get_exported_func_addr(0xb5d5f64e, "sysPrxForUser"))
	{
		return vm::ptr<error_code(u64, vm::ptr<u32>)>{vm::cast(entry)}(ppu, flags, address);
	}

	ppu.state += cpu_flag::wait;

	sysPrxForUser.notice("sys_mmapper_get_shared_memory_area(flags=0x%llx, address=*0x%x)", flags, address);

	if ((flags & 0xf00) != 0x200 || ((flags & 0xff) != 0 && (flags & 0xff) != 0xf))
	{
		return CELL_EINVAL;
	}

	const std::unique_lock savestate_lock{ g_fxo->get<hle_locks_t>(), std::try_to_lock };

	if (!savestate_lock)
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	auto& area = *vm::_ptr<atomic_be_t<u32>>(s_shared_memory_area.addr());

	if (const u32 made = area.load())
	{
		*address = made;
		return CELL_OK;
	}

	const vm::var<u32> made(0);

	if (const auto ret = sys_mmapper_allocate_address(ppu, 0x10000000, 0x20f, 0x10000000, made))
	{
		// Another caller's may have taken the last area
		if (const u32 theirs = area.load())
		{
			*address = theirs;
			return CELL_OK;
		}

		return ret;
	}

	// Published, or another caller's was first: this one given back
	const u32 mine = *made;
	const u32 old = area.compare_and_swap(0, mine);

	if (old)
	{
		ensure(sys_mmapper_free_address(ppu, mine) == CELL_OK);
	}

	*address = old ? old : mine;
	return CELL_OK;
}

error_code sys_mmapper_allocate_memory(ppu_thread& ppu, u32 size, u64 flags, vm::ptr<u32> mem_id)
{
	sysPrxForUser.notice("sys_mmapper_allocate_memory(size=0x%x, flags=0x%llx, mem_id=*0x%x)", size, flags, mem_id);

	return sys_mmapper_allocate_shared_memory(ppu, SYS_MMAPPER_NO_SHM_KEY, size, flags, mem_id);
}

error_code sys_mmapper_allocate_memory_from_container(ppu_thread& ppu, u32 size, u32 cid, u64 flags, vm::ptr<u32> mem_id)
{
	sysPrxForUser.notice("sys_mmapper_allocate_memory_from_container(size=0x%x, cid=0x%x, flags=0x%llx, mem_id=*0x%x)", size, cid, flags, mem_id);

	return sys_mmapper_allocate_shared_memory_from_container(ppu, SYS_MMAPPER_NO_SHM_KEY, size, cid, flags, mem_id);
}

error_code sys_mmapper_map_memory(ppu_thread& ppu, u32 addr, u32 mem_id, u64 flags)
{
	sysPrxForUser.notice("sys_mmapper_map_memory(addr=0x%x, mem_id=0x%x, flags=0x%llx)", addr, mem_id, flags);

	return sys_mmapper_map_shared_memory(ppu, addr, mem_id, flags);
}

error_code sys_mmapper_unmap_memory(ppu_thread& ppu, u32 addr, vm::ptr<u32> mem_id)
{
	sysPrxForUser.notice("sys_mmapper_unmap_memory(addr=0x%x, mem_id=*0x%x)", addr, mem_id);

	return sys_mmapper_unmap_shared_memory(ppu, addr, mem_id);
}

error_code sys_mmapper_free_memory(ppu_thread& ppu, u32 mem_id)
{
	sysPrxForUser.notice("sys_mmapper_free_memory(mem_id=0x%x)", mem_id);

	return sys_mmapper_free_shared_memory(ppu, mem_id);
}

extern void sysPrxForUser_sys_mmapper_init()
{
	REG_VAR(sysPrxForUser, s_shared_memory_area).flag(MFF_HIDDEN);
	// leave the guest import unimplemented for now, LLE sysutil waits for the missing vsh service otherwise

	REG_FUNC(sysPrxForUser, sys_mmapper_allocate_memory);
	REG_FUNC(sysPrxForUser, sys_mmapper_allocate_memory_from_container);
	REG_FUNC(sysPrxForUser, sys_mmapper_map_memory);
	REG_FUNC(sysPrxForUser, sys_mmapper_unmap_memory);
	REG_FUNC(sysPrxForUser, sys_mmapper_free_memory);
}
