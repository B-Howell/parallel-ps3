#include "stdafx.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/IdManager.h"

#include "sceNp.h"
#include "sceNpUtil.h"
#include "Emu/Cell/timers.hpp"

LOG_CHANNEL(sceNpUtil);

// The bandwidth test is a fake one (no test is run, the result is fixed):
// it is "running" for 100 ms of the guest's time from its start, or until
// it is aborted. That is one word - no thread that slept a millisecond at a
// time, no lock, nothing waited for.
struct sce_np_util_manager
{
	// 0: no test; else the guest time it started at << 1 | aborted
	atomic_t<u64> test = 0;

	static constexpr u64 test_us = 100'000;
};

error_code sceNpUtilBandwidthTestInitStart([[maybe_unused]] ppu_thread& ppu, u32 prio, u32 stack)
{
	sceNpUtil.warning("sceNpUtilBandwidthTestInitStart(prio=%d, stack=%d)", prio, stack);

	auto& util_manager = g_fxo->get<sce_np_util_manager>();

	if (!util_manager.test.compare_and_swap_test(0, std::max<u64>(get_guest_system_time(), 1) << 1))
	{
		return SCE_NP_ERROR_ALREADY_INITIALIZED;
	}

	return CELL_OK;
}

error_code sceNpUtilBandwidthTestGetStatus()
{
	sceNpUtil.notice("sceNpUtilBandwidthTestGetStatus()");

	const u64 test = g_fxo->get<sce_np_util_manager>().test.load();

	if (!test)
	{
		return SCE_NP_ERROR_NOT_INITIALIZED;
	}

	const bool done = (test & 1) || get_guest_system_time() - (test >> 1) > sce_np_util_manager::test_us;
	return not_an_error(done ? SCE_NP_UTIL_BANDWIDTH_TEST_STATUS_FINISHED : SCE_NP_UTIL_BANDWIDTH_TEST_STATUS_RUNNING);
}

error_code sceNpUtilBandwidthTestShutdown([[maybe_unused]] ppu_thread& ppu, vm::ptr<SceNpUtilBandwidthTestResult> result)
{
	sceNpUtil.warning("sceNpUtilBandwidthTestShutdown(result=*0x%x)", result);

	if (!g_fxo->get<sce_np_util_manager>().test.exchange(0))
	{
		return SCE_NP_ERROR_NOT_INITIALIZED;
	}

	if (result) // TODO: what happens when this is null ?
	{
		SceNpUtilBandwidthTestResult test_result{};
		test_result.upload_bps = 100'000'000.0;
		test_result.download_bps = 100'000'000.0;
		test_result.result = CELL_OK;
		*result = test_result;
	}

	return CELL_OK;
}

error_code sceNpUtilBandwidthTestAbort()
{
	sceNpUtil.todo("sceNpUtilBandwidthTestAbort()");

	auto& util_manager = g_fxo->get<sce_np_util_manager>();

	// TODO: cellHttpTransactionAbortConnection();

	for (u64 test = util_manager.test.load();; )
	{
		if (!test)
		{
			return SCE_NP_ERROR_NOT_INITIALIZED;
		}

		if (util_manager.test.compare_exchange(test, test | 1))
		{
			return CELL_OK;
		}
	}
}

DECLARE(ppu_module_manager::sceNpUtil)("sceNpUtil", []()
{
	REG_FUNC(sceNpUtil, sceNpUtilBandwidthTestInitStart);
	REG_FUNC(sceNpUtil, sceNpUtilBandwidthTestShutdown);
	REG_FUNC(sceNpUtil, sceNpUtilBandwidthTestGetStatus);
	REG_FUNC(sceNpUtil, sceNpUtilBandwidthTestAbort);
});
