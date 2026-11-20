#pragma once

#include <retro_atomic.h>

#include "gcm_io_pages.h"

#include "Emu/RSX/GCM.h"
#include "Emu/Memory/vm_ptr.h"

enum CellGcmError : u32
{
	CELL_GCM_ERROR_FAILURE           = 0x802100ff,
	CELL_GCM_ERROR_NO_IO_PAGE_TABLE  = 0x80210001,
	CELL_GCM_ERROR_INVALID_ENUM      = 0x80210002,
	CELL_GCM_ERROR_INVALID_VALUE     = 0x80210003,
	CELL_GCM_ERROR_INVALID_ALIGNMENT = 0x80210004,
	CELL_GCM_ERROR_ADDRESS_OVERWRAP  = 0x80210005,
};

struct CellGcmOffsetTable
{
	vm::bptr<u16> ioAddress;
	vm::bptr<u16> eaAddress;
};

struct gcm_config
{
	u32 zculls_addr;
	vm::ptr<CellGcmDisplayInfo> gcm_buffers = vm::null;
	u32 tiles_addr;
	u32 ctxt_addr;

	CellGcmConfig current_config;
	CellGcmContextData current_context;
	gcmInfo gcm_info;

	CellGcmOffsetTable offsetTable{};
	u16 IoMapTable[0xC00]{};

	// The IO pages a map or unmap is working on - its syscall, then the
	// offset tables' and IoMapTable's entries for them - are marked here
	// (gcm_io_pages.c): no lock, no turn. Marked, they are that map's or unmap's alone; a search for free
	// pages passes them over, and another map or unmap of one of them
	// meanwhile is refused
	retro_atomic_64_t io_pending[GCM_IO_PAGES / 64]{};

	// cellGcmMapLocalMemory's claim, once
	retro_atomic_int_t local_mapped{};

	u64 system_mode = 0;
	u32 local_size = 0;
	u32 local_addr = 0;

	// The main memory cellGcmReserveIoMapSize has kept back, checked and
	// changed in one compare-and-swap
	retro_atomic_int_t reserved_size{0};

	u32 reserved_now() const { return static_cast<u32>(retro_atomic_load_acquire_int(&reserved_size)); }

	// @size more, if it leaves the total no more than @limit
	bool reserve(u32 size, u32 limit)
	{
		for (;;)
		{
			const u32 reserved = reserved_now();

			if (size > limit - reserved)
				return false;

			if (retro_atomic_cas_int(&reserved_size, static_cast<int>(reserved), static_cast<int>(reserved + size)))
				return true;
		}
	}

	// @size less, if that much is kept back
	bool unreserve(u32 size)
	{
		for (;;)
		{
			const u32 reserved = reserved_now();

			if (size > reserved)
				return false;

			if (retro_atomic_cas_int(&reserved_size, static_cast<int>(reserved), static_cast<int>(reserved - size)))
				return true;
		}
	}
};

void InitOffsetTable();
