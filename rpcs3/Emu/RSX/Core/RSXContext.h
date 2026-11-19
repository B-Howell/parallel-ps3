#pragma once

#include <util/types.hpp>
#include <retro_atomic.h>
#include "Emu/Cell/lv2/sys_rsx.h"
#include "Emu/RSX/GCM.h"
#include "RSXIOMap.hpp"

namespace rsx
{
	namespace gcm
	{
		enum limits
		{
			tiles_count = 15,
			zculls_count = 8
		};
	}

	struct GCM_tile_reference
	{
		u32 base_address = 0;
		const GcmTileInfo* tile = nullptr;

		operator bool() const
		{
			return !!tile;
		}

		utils::address_range32 tile_align(const rsx::address_range32& range) const;
	};

	struct GCM_context
	{
		RsxDmaControl* ctrl = nullptr;
		u32 dma_address{ 0 };
		rsx_iomap_table iomap_table;

		GcmTileInfo tiles[gcm::limits::tiles_count];
		GcmZcullInfo zculls[gcm::limits::zculls_count];

		RsxDisplayInfo display_buffers[8];
		u32 display_buffers_count{ 0 };
		u32 current_display_buffer{ 0 };

		// What the sys_rsx syscalls change of it - the IO map (its entries
		// atomic, a map or unmap under the memory map's writer lock), tiles,
		// zcull regions, display buffers - is changed with no turn; two
		// threads setting the same tile at once is the game's race. The
		// device's address is kept by compare-and-swap (sys_rsx.cpp)
		u32 device_addr{ 0 };
		u32 label_addr{ 0 };
		u32 main_mem_size{ 0 };
		u32 local_mem_size{ 0 };
		u32 rsx_event_port{ 0 };
		u32 driver_info{ 0 };

		retro_atomic_64_t unsent_gcm_events{0}; // Unsent event bits when aborting RSX/VBLANK thread (will be sent on savestate load); saved as the u64 it was

		GCM_tile_reference get_tiled_memory_region(const utils::address_range32& range) const;
	};
}
