#include "stdafx.h"
#include "Emu/Cell/PPUModule.h"
#include "cellSysutil.h"
#include "sceNp.h"
#include "Emu/IdManager.h"

LOG_CHANNEL(cellSysutilNpEula);

enum SceNpEulaStatus
{
	SCE_NP_EULA_UNKNOWN          = 0,
	SCE_NP_EULA_ACCEPTED         = 1,
	SCE_NP_EULA_ALREADY_ACCEPTED = 2,
	SCE_NP_EULA_REJECTED         = 3,
	SCE_NP_EULA_ABORTED          = 4,
	SCE_NP_EULA_ERROR            = 5,
};

using SceNpEulaVersion = u32;
using SceNpEulaCheckEulaStatusCallback = void(SceNpEulaStatus status, u32 errorCode, SceNpEulaVersion version, vm::ptr<void> arg);

struct sceNpEulaCallbacksRegistered
{
	retro_atomic_int_t status{static_cast<int>(SCE_NP_EULA_UNKNOWN)};
	retro_atomic_int_t sceNpEulaCheckEulaStatus_callback_registered{0}; // 0 or 1
	retro_atomic_int_t sceNpEulaShowCurrentEula_callback_registered{0}; // 0 or 1
};

error_code sceNpEulaCheckEulaStatus(vm::cptr<SceNpCommunicationId> communicationId, u32 arg2, u64 arg3, vm::ptr<SceNpEulaCheckEulaStatusCallback> cbFunc, vm::ptr<void> cbFuncArg)
{
	cellSysutilNpEula.warning("sceNpEulaCheckEulaStatus(communicationId=*0x%x, arg2=0x%x, arg3=0x%x, cbFunc=*0x%x, cbFuncArg=*0x%x)", communicationId, arg2, arg3, cbFunc, cbFuncArg);

	if (!communicationId || !cbFunc)
	{
		return SCE_NP_EULA_ERROR_INVALID_ARGUMENT;
	}

	auto& cb_infos = g_fxo->get<sceNpEulaCallbacksRegistered>();

	if ((retro_atomic_load_acquire_int(&cb_infos.sceNpEulaCheckEulaStatus_callback_registered) != 0) || (retro_atomic_load_acquire_int(&cb_infos.sceNpEulaShowCurrentEula_callback_registered) != 0))
	{
		return SCE_NP_EULA_ERROR_ALREADY_INITIALIZED;
	}

	retro_atomic_store_release_int(&cb_infos.sceNpEulaCheckEulaStatus_callback_registered, 1);
	retro_atomic_store_release_int(&cb_infos.status, static_cast<int>(SCE_NP_EULA_ALREADY_ACCEPTED));

	sysutil_register_cb([=](ppu_thread& cb_ppu) -> s32
	{
		auto& cb_infos = g_fxo->get<sceNpEulaCallbacksRegistered>();
		cbFunc(cb_ppu, static_cast<SceNpEulaStatus>(retro_atomic_load_acquire_int(&cb_infos.status)), CELL_OK, 1, cbFuncArg);
		retro_atomic_store_release_int(&cb_infos.sceNpEulaCheckEulaStatus_callback_registered, 0);
		return 0;
	});

	return CELL_OK;
}

error_code sceNpEulaAbort()
{
	cellSysutilNpEula.warning("sceNpEulaAbort()");

	auto& cb_infos = g_fxo->get<sceNpEulaCallbacksRegistered>();

	if (retro_atomic_load_acquire_int(&cb_infos.sceNpEulaCheckEulaStatus_callback_registered) == 0 && retro_atomic_load_acquire_int(&cb_infos.sceNpEulaShowCurrentEula_callback_registered) == 0)
	{
		return SCE_NP_EULA_ERROR_NOT_INITIALIZED;
	}

	// It would forcefully abort the dialog/process of getting the eula but since we don't show the dialog, just alter the status returned
	retro_atomic_store_release_int(&cb_infos.status, static_cast<int>(SCE_NP_EULA_ABORTED));

	return CELL_OK;
}

// Seen on: Resistance 3, Uncharted 2
error_code sceNpEulaShowCurrentEula(vm::cptr<SceNpCommunicationId> communicationId, u64 arg2, vm::ptr<CellSysutilCallback> cbFunc, vm::ptr<void> cbFuncArg)
{
	cellSysutilNpEula.todo("sceNpEulaShowCurrentEula(communicationId=*0x%x, arg2=0x%x, cbFunc=*0x%x, cbFuncArg=*0x%x)", communicationId, arg2, cbFunc, cbFuncArg);

	if (!communicationId || !cbFunc)
	{
		return SCE_NP_EULA_ERROR_INVALID_ARGUMENT;
	}

	auto& cb_infos = g_fxo->get<sceNpEulaCallbacksRegistered>();

	if ((retro_atomic_load_acquire_int(&cb_infos.sceNpEulaCheckEulaStatus_callback_registered) != 0) || (retro_atomic_load_acquire_int(&cb_infos.sceNpEulaShowCurrentEula_callback_registered) != 0))
	{
		return SCE_NP_EULA_ERROR_ALREADY_INITIALIZED;
	}

	// Call callback (Unknown parameters)

	return CELL_OK;
}

DECLARE(ppu_module_manager::cellSysutilNpEula)("cellSysutilNpEula", []()
{
	REG_FUNC(cellSysutilNpEula, sceNpEulaCheckEulaStatus);
	REG_FUNC(cellSysutilNpEula, sceNpEulaAbort);
	REG_FUNC(cellSysutilNpEula, sceNpEulaShowCurrentEula);
});
