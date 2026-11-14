#include "stdafx.h"
#include "util/sysinfo.hpp"
#include "util/v128.hpp"
#include "Emu/Cell/lv2/sys_process.h"
#include "Emu/Memory/vm_ptr.h"
#include "Emu/Cell/ErrorCodes.h"
#include "Emu/System.h"
#include "Emu/system_utils.hpp"
#include "Emu/IdManager.h"
#include "Utilities/StrUtil.h"

#include "Emu/Cell/timers.hpp"
#include "sys_game.h"

#include <retro_atomic.h>

LOG_CHANNEL(sys_game);

struct system_sw_version
{
	system_sw_version()
	{
		f64 version_f = 0;
		if (!try_to_float(&version_f, utils::get_firmware_version(), 0.0f, 99.9999f))
			sys_game.error("Error parsing firmware version");
		retro_atomic_store_release_64(&version, static_cast<s64>(version_f * 10000));
	}

	system_sw_version(const system_sw_version&) = delete;

	system_sw_version& operator=(const system_sw_version&) = delete;

	~system_sw_version() = default;

	retro_atomic_64_t version{};
};

struct board_storage
{
public:
	bool read(u8* buffer)
	{
		if (!buffer)
			return false;

		const retro_u128_t data = retro_atomic_load_acquire_128(&storage);
		memcpy(buffer, &data, size);

		return true;
	}

	bool write(const u8* buffer)
	{
		if (!buffer)
			return false;

		// The bytes as they are, as the big-endian v128 kept them
		retro_u128_t data;
		memcpy(&data, buffer, size);
		retro_atomic_store_release_128(&storage, data);
		retro_atomic_store_release_int(&written, 1);

		return true;
	}

	board_storage()
	{
		retro_u128_t data;
		memset(&data, -1, size);
		if (fs::file file; file.open(file_path, fs::read))
			file.read(&data, std::min(file.size(), size));
		retro_atomic_128_init(&storage, data);
	}

	board_storage(const board_storage&) = delete;

	board_storage& operator=(const board_storage&) = delete;

	~board_storage()
	{
		if (retro_atomic_load_acquire_int(&written))
		{
			if (fs::file file; file.open(file_path, fs::create + fs::write + fs::lock))
			{
				const retro_u128_t data = retro_atomic_load_acquire_128(&storage);
				file.write(&data, size);
				file.trunc(size);
			}
		}
	}

private:
	retro_atomic_128_t storage; // the 16 bytes as the guest wrote them
	retro_atomic_int_t written{};
	const std::string file_path = rpcs3::utils::get_hdd1_dir() + "/caches/board_storage.bin";
	static constexpr u64 size = sizeof(v128);
};

// The game's watchdog: restarts the game when it is not cleared within the
// timeout. No thread of its own: retro_run looks at it once a frame
// (lv2_watchdog_tick), on the guest's clock, which stops while the frontend
// is paused. The syscalls only change its words.
struct watchdog_t
{
	// Bit 0: needs restart (start counting again), bit 1: active, bit 2:
	// given up (the game is exiting); the high 32 bits: the timeout, in us
	static constexpr s64 needs_restart = 1, active = 2, aborted = 4;
	retro_atomic_64_t control{};

	// The guest's time the count started at, and retro_run's last look
	retro_atomic_64_t start{};
	u64 last_tick = 0;
};

bool lv2_watchdog_step(retro_atomic_64_t* control, retro_atomic_64_t* start, u64 paused_for)
{
	const u64 now = get_guest_system_time();
	const s64 old = retro_atomic_load_acquire_64(control);

	if ((old & watchdog_t::aborted) || !(old & watchdog_t::active))
	{
		return false;
	}

	if (paused_for)
	{
		retro_atomic_fetch_add_64(start, static_cast<s64>(paused_for));
	}

	if (old & watchdog_t::needs_restart)
	{
		if (retro_atomic_cas_64(control, old, old & ~watchdog_t::needs_restart))
		{
			retro_atomic_store_release_64(start, static_cast<s64>(now));
		}

		return false;
	}

	const u64 timeout = static_cast<u64>(old) >> 32;

	return now - static_cast<u64>(retro_atomic_load_acquire_64(start)) >= timeout
		&& retro_atomic_cas_64(control, old, old | watchdog_t::aborted);
}

void lv2_watchdog_tick()
{
	const auto watchdog = g_fxo->try_get<watchdog_t>();

	if (!watchdog)
	{
		return;
	}

	const u64 now = get_guest_system_time();
	const u64 last = std::exchange(watchdog->last_tick, now);

	// Paused inside the emulator, not by the frontend: not counted
	const u64 paused_for = Emu.GetStatus(false) == system_state::paused ? now - std::min(last, now) : 0;

	if (lv2_watchdog_step(&watchdog->control, &watchdog->start, paused_for))
	{
		sys_game.success("Watchdog timeout! Restarting the game...");

		Emu.CallFromMainThread([]()
		{
			Emu.Restart(false);
		});
	}
}

void abort_lv2_watchdog()
{
	if (auto watchdog = g_fxo->try_get<watchdog_t>())
	{
		sys_game.notice("Aborting the LV2 watchdog...");
		retro_atomic_fetch_or_64(&watchdog->control, watchdog_t::aborted);
	}
}

error_code _sys_game_watchdog_start(u32 timeout)
{
	sys_game.trace("sys_game_watchdog_start(timeout=%d)", timeout);

	// According to disassembly
	timeout *= 1'000'000;
	timeout &= -64;

	auto& control = g_fxo->get<watchdog_t>().control;

	for (;;)
	{
		const s64 old = retro_atomic_load_acquire_64(&control);

		if (old & watchdog_t::active)
		{
			return CELL_EABORT;
		}

		const s64 next = static_cast<s64>((u64{timeout} << 32) | static_cast<u64>((old & watchdog_t::aborted) | watchdog_t::needs_restart | watchdog_t::active));

		if (retro_atomic_cas_64(&control, old, next))
		{
			return CELL_OK;
		}
	}
}

error_code _sys_game_watchdog_stop()
{
	sys_game.trace("sys_game_watchdog_stop()");

	retro_atomic_fetch_and_64(&g_fxo->get<watchdog_t>().control, ~watchdog_t::active);

	return CELL_OK;
}

error_code _sys_game_watchdog_clear()
{
	sys_game.trace("sys_game_watchdog_clear()");

	auto& control = g_fxo->get<watchdog_t>().control;

	for (;;)
	{
		const s64 old = retro_atomic_load_acquire_64(&control);

		if (!(old & watchdog_t::active) || (old & watchdog_t::needs_restart) || retro_atomic_cas_64(&control, old, old | watchdog_t::needs_restart))
		{
			return CELL_OK;
		}
	}
}

error_code _sys_game_set_system_sw_version(u64 version)
{
	sys_game.trace("sys_game_set_system_sw_version(version=%d)", version);

	if (!g_ps3_process_info.has_root_perm())
		return CELL_ENOSYS;

	retro_atomic_store_release_64(&g_fxo->get<system_sw_version>().version, static_cast<s64>(version));

	return CELL_OK;
}

u64 _sys_game_get_system_sw_version()
{
	sys_game.trace("sys_game_get_system_sw_version()");

	return static_cast<u64>(retro_atomic_load_acquire_64(&g_fxo->get<system_sw_version>().version));
}

error_code _sys_game_board_storage_read(vm::ptr<u8> buffer, vm::ptr<u8> status)
{
	sys_game.trace("sys_game_board_storage_read(buffer=*0x%x, status=*0x%x)", buffer, status);

	if (!buffer || !status)
	{
		return CELL_EFAULT;
	}

	*status = g_fxo->get<board_storage>().read(buffer.get_ptr()) ? 0x00 : 0xFF;

	return CELL_OK;
}

error_code _sys_game_board_storage_write(vm::ptr<u8> buffer, vm::ptr<u8> status)
{
	sys_game.trace("sys_game_board_storage_write(buffer=*0x%x, status=*0x%x)", buffer, status);

	if (!buffer || !status)
	{
		return CELL_EFAULT;
	}

	*status = g_fxo->get<board_storage>().write(buffer.get_ptr()) ? 0x00 : 0xFF;

	return CELL_OK;
}

error_code _sys_game_get_rtc_status(vm::ptr<s32> status)
{
	sys_game.trace("sys_game_get_rtc_status(status=*0x%x)", status);

	if (!status)
	{
		return CELL_EFAULT;
	}

	*status = 0;

	return CELL_OK;
}
