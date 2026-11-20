#include "stdafx.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Memory/guest_atomic.h"

LOG_CHANNEL(sysPrxForUser);

void sys_spinlock_initialize(vm::ptr<be_t<u32>> lock)
{
	sysPrxForUser.trace("sys_spinlock_initialize(lock=*0x%x)", lock);

	if (guest_atomic_load_be32(lock.get_ptr()))
	{
		guest_atomic_store_be32(lock.get_ptr(), 0);
	}
}

error_code sys_spinlock_lock(ppu_thread& ppu, vm::ptr<be_t<u32>> lock)
{
	sysPrxForUser.trace("sys_spinlock_lock(lock=*0x%x)", lock);

	// Try to exchange with 0xabadcafe, repeat until exchanged with 0: the
	// game's own spinlock, which its code may also release with a plain store
	while (guest_atomic_load_be32(lock.get_ptr()) || guest_atomic_exchange_be32(lock.get_ptr(), 0xabadcafe))
	{
		if (ppu.test_stopped())
		{
			ppu.state += cpu_flag::again;
			return {};
		}
	}

	return CELL_OK;
}

error_code sys_spinlock_trylock(vm::ptr<be_t<u32>> lock)
{
	sysPrxForUser.trace("sys_spinlock_trylock(lock=*0x%x)", lock);

	if (guest_atomic_load_be32(lock.get_ptr()) || guest_atomic_exchange_be32(lock.get_ptr(), 0xabadcafe))
	{
		return not_an_error(CELL_EBUSY);
	}

	return CELL_OK;
}

void sys_spinlock_unlock(vm::ptr<be_t<u32>> lock)
{
	sysPrxForUser.trace("sys_spinlock_unlock(lock=*0x%x)", lock);

	guest_atomic_store_be32(lock.get_ptr(), 0);
}

void sysPrxForUser_sys_spinlock_init()
{
	REG_FUNC(sysPrxForUser, sys_spinlock_initialize);
	REG_FUNC(sysPrxForUser, sys_spinlock_lock);
	REG_FUNC(sysPrxForUser, sys_spinlock_trylock);
	REG_FUNC(sysPrxForUser, sys_spinlock_unlock);
}
