#include "stdafx.h"
#include "sys_semaphore.h"

#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"

#include "util/asm.hpp"

LOG_CHANNEL(sys_semaphore);

lv2_sema::lv2_sema(utils::serial& ar)
	: protocol(ar)
	, key(ar)
	, name(ar)
	, max(ar)
{
	retro_atomic_store_release_int(&val, ar.pop<s32>());
}

std::function<void(void*)> lv2_sema::load(utils::serial& ar)
{
	return load_func(make_shared<lv2_sema>(stx::exact_t<utils::serial&>(ar)));
}

void lv2_sema::save(utils::serial& ar)
{
	USING_SERIALIZATION_VERSION(lv2_sync);
	ar(protocol, key, name, max, std::max<s32>(retro_atomic_load_acquire_int(&val), 0));
}

error_code sys_semaphore_create(ppu_thread& ppu, vm::ptr<u32> sem_id, vm::ptr<sys_semaphore_attribute_t> attr, s32 initial_val, s32 max_val)
{
	ppu.state += cpu_flag::wait;

	sys_semaphore.trace("sys_semaphore_create(sem_id=*0x%x, attr=*0x%x, initial_val=%d, max_val=%d)", sem_id, attr, initial_val, max_val);

	if (!sem_id || !attr)
	{
		return CELL_EFAULT;
	}

	if (max_val <= 0 || initial_val > max_val || initial_val < 0)
	{
		sys_semaphore.error("sys_semaphore_create(): invalid parameters (initial_val=%d, max_val=%d)", initial_val, max_val);
		return CELL_EINVAL;
	}

	const auto _attr = *attr;

	const u32 protocol = _attr.protocol;

	if (protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_PRIORITY)
	{
		sys_semaphore.error("sys_semaphore_create(): unknown protocol (0x%x)", protocol);
		return CELL_EINVAL;
	}

	const u64 ipc_key = lv2_obj::get_key(_attr);

	if (ipc_key)
	{
		sys_semaphore.warning("sys_semaphore_create(sem_id=*0x%x, attr=*0x%x, initial_val=%d, max_val=%d): IPC=0x%016x", sem_id, attr, initial_val, max_val, ipc_key);
	}

	if (auto error = lv2_obj::create<lv2_sema>(_attr.pshared, ipc_key, _attr.flags, [&]
	{
		return make_shared<lv2_sema>(protocol, ipc_key, _attr.name_u64, max_val, initial_val);
	}))
	{
		return error;
	}

	ppu.check_state();

	*sem_id = idm::last_id<lv2_sema>();
	return CELL_OK;
}

error_code sys_semaphore_destroy(ppu_thread& ppu, u32 sem_id)
{
	ppu.state += cpu_flag::wait;

	sys_semaphore.trace("sys_semaphore_destroy(sem_id=0x%x)", sem_id);

	const auto sem = idm::withdraw<lv2_obj, lv2_sema>(sem_id, [](lv2_sema& sema) -> CellError
	{
		if (retro_atomic_load_acquire_int(&sema.val) < 0)
		{
			return CELL_EBUSY;
		}

		lv2_obj::on_id_destroy(sema, sema.key);
		return {};
	});

	if (!sem)
	{
		return CELL_ESRCH;
	}

	if (sem->key)
	{
		sys_semaphore.warning("sys_semaphore_destroy(sem_id=0x%x): IPC=0x%016x", sem_id, sem->key);
	}

	if (sem.ret)
	{
		return sem.ret;
	}

	return CELL_OK;
}

error_code sys_semaphore_wait(ppu_thread& ppu, u32 sem_id, u64 timeout)
{
	ppu.state += cpu_flag::wait;

	sys_semaphore.trace("sys_semaphore_wait(sem_id=0x%x, timeout=0x%llx)", sem_id, timeout);

	// Free, as it mostly is: found and taken with no lock and no reference
	// counted. Taking a count that is there makes no waiter, which is all
	// sys_semaphore_destroy asks about, so this needs no second look: it
	// is as if it came before a destroy that ran meanwhile.
	if (const auto pinned = idm::pin<lv2_obj, lv2_sema>(sem_id))
	{
		const s32 val = retro_atomic_load_acquire_int(&pinned->val);

		if (val > 0 && retro_atomic_cas_int(&pinned->val, val, val - 1))
		{
			return CELL_OK;
		}
	}
	else
	{
		return CELL_ESRCH;
	}

	// The thread may sleep on it, so it keeps a reference
	struct
	{
		stx::shared_ptr<lv2_sema> ptr;
		s32 ret = 0; // 1: has a count, 0: sleeps, -1: no such semaphore

		explicit operator bool() const { return !!ptr; }
		lv2_sema* operator->() const { return ptr.get(); }
	}
	sem{idm::hold<lv2_obj, lv2_sema>(sem_id)};

	if (sem)
	{
		sem.ret = [&, notify = lv2_obj::notify_all_t()](lv2_sema& sema) -> s32
		{
			const s32 val = retro_atomic_load_acquire_int(&sema.val);

			if (val > 0)
			{
				if (retro_atomic_cas_int(&sema.val, val, val - 1))
				{
					return 1;
				}
			}

			lv2_obj::prepare_for_sleep(ppu);

			// The thread is a waiter once it is on the queue, and may be
			// woken before it has gone to sleep: that sleep is then cancelled
			// (lv2_obj::sleep gives false), as sys_mutex has it
			ppu.cancel_sleep = 1;

			// The count and the queue change in the semaphore's turn; the
			// scheduler is told on this thread, after
			const s32 entered = turn_call(&sema.turns, [&]
			{
				const s32 got = sema.enter(sem_id);

				if (!got)
				{
					lv2_obj::emplace(sema.sq, &ppu);
				}

				return got;
			});

			if (entered)
			{
				ppu.cancel_sleep = 0;
				return entered;
			}

			if (!sema.sleep(ppu, timeout))
			{
				// Given a count before it went to sleep
				ppu.cancel_sleep = 0;
				return 1;
			}

			if (ppu.cancel_sleep != 1)
			{
				notify.cleanup();
			}

			ppu.cancel_sleep = 0;
			return 0;
		}(*sem.ptr);
	}

	if (!sem || sem.ret < 0)
	{
		return CELL_ESRCH;
	}

	if (sem.ret)
	{
		return CELL_OK;
	}

	ppu.gpr[3] = CELL_OK;

	while (auto state = +ppu.state)
	{
		if (state & cpu_flag::signal && ppu.state.test_and_reset(cpu_flag::signal))
		{
			break;
		}

		if (is_stopped(state))
		{
			const bool queued = turn_call(&sem->turns, [&]
			{
				for (auto cpu = +sem->sq; cpu; cpu = cpu->next_cpu)
				{
					if (cpu == &ppu)
					{
						return true;
					}
				}

				return false;
			});

			if (queued)
			{
				ppu.state += cpu_flag::again;
				return {};
			}

			break;
		}

		for (usz i = 0; cpu_flag::signal - ppu.state && i < 50; i++)
		{
			busy_wait(500);
		}

		if (ppu.state & cpu_flag::signal)
 		{
			continue;
		}

		if (timeout)
		{
			if (lv2_obj::wait_timeout(timeout, &ppu))
			{
				// Wait for rescheduling
				if (ppu.check_state())
				{
					continue;
				}

				ppu.state += cpu_flag::wait;

				const bool timed_out = turn_call(&sem->turns, [&]
				{
					if (!sem->unqueue(sem->sq, &ppu))
					{
						return false;
					}

					// Give back the count the thread was waiting with
					ensure(0 > [&]
					{
						for (;;)
						{
							const s32 val = retro_atomic_load_acquire_int(&sem->val);

							if (val >= 0 || retro_atomic_cas_int(&sem->val, val, val + 1))
							{
								return val;
							}
						}
					}());

					return true;
				});

				if (timed_out)
				{
					ppu.gpr[3] = CELL_ETIMEDOUT;
				}

				break;
			}
		}
		else
		{
			ppu.state.wait(state);
		}
	}

	return not_an_error(ppu.gpr[3]);
}

error_code sys_semaphore_trywait(ppu_thread& ppu, u32 sem_id)
{
	ppu.state += cpu_flag::wait;

	sys_semaphore.trace("sys_semaphore_trywait(sem_id=0x%x)", sem_id);

	// Found with no lock; see sys_semaphore_wait for why that is enough
	const auto sem = idm::pin<lv2_obj, lv2_sema>(sem_id);

	if (!sem)
	{
		return CELL_ESRCH;
	}

	// Take a count if there is one
	if (![&]
	{
		for (;;)
		{
			const s32 val = retro_atomic_load_acquire_int(&sem->val);

			if (val <= 0)
			{
				return false;
			}

			if (retro_atomic_cas_int(&sem->val, val, val - 1))
			{
				return true;
			}
		}
	}())
	{
		return not_an_error(CELL_EBUSY);
	}

	return CELL_OK;
}

error_code sys_semaphore_post(ppu_thread& ppu, u32 sem_id, s32 count)
{
	ppu.state += cpu_flag::wait;

	sys_semaphore.trace("sys_semaphore_post(sem_id=0x%x, count=%d)", sem_id, count);

	// No waiters, as mostly: found and counted up with no lock and no
	// reference counted (see sys_semaphore_wait)
	if (const auto pinned = idm::pin<lv2_obj, lv2_sema>(sem_id))
	{
		const s32 val = retro_atomic_load_acquire_int(&pinned->val);

		if (val >= 0 && count > 0 && count <= pinned->max - val)
		{
			if (retro_atomic_cas_int(&pinned->val, val, val + count))
			{
				return CELL_OK;
			}
		}
	}
	else
	{
		return CELL_ESRCH;
	}

	if (count <= 0)
	{
		return CELL_EINVAL;
	}

	// Threads to wake: their queue is walked in the semaphore's turn, with a
	// reference kept
	const auto sem = idm::hold<lv2_obj, lv2_sema>(sem_id);

	if (!sem)
	{
		return CELL_ESRCH;
	}

	lv2_obj::notify_all_t notify;

	// What the turn found: 1 posted, 0 over the most, -1 to be done again
	const s32 posted = turn_call(&sem->turns, [&]() -> s32
	{
		for (auto cpu = +sem->sq; cpu; cpu = cpu->next_cpu)
		{
			if (static_cast<ppu_thread*>(cpu)->state & cpu_flag::again)
			{
				return -1;
			}
		}

		s32 val;
		bool ok;

		for (;;)
		{
			val = retro_atomic_load_acquire_int(&sem->val);
			ok = count + 0u <= sem->max + 0u - val;

			if (!ok || retro_atomic_cas_int(&sem->val, val, val + count))
			{
				break;
			}
		}

		if (!ok)
		{
			return 0;
		}

		// Wake threads, in the turn: a thread taken off the queue is woken
		// before its own turn (its timeout's) can see it gone. Waking a
		// thread is for any thread to do; only going to sleep is the
		// thread's own, and that is done out of the turn
		const s32 to_awake = std::min<s32>(-std::min<s32>(val, 0), count);

		for (s32 i = 0; i < to_awake; i++)
		{
			sem->append((ensure(sem->schedule<ppu_thread>(sem->sq, sem->protocol))));
		}

		if (to_awake > 0)
		{
			lv2_obj::awake_all();
		}

		return 1;
	});

	if (posted < 0)
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	if (!posted)
	{
		return not_an_error(CELL_EBUSY);
	}

	return CELL_OK;
}

error_code sys_semaphore_get_value(ppu_thread& ppu, u32 sem_id, vm::ptr<s32> count)
{
	ppu.state += cpu_flag::wait;

	sys_semaphore.trace("sys_semaphore_get_value(sem_id=0x%x, count=*0x%x)", sem_id, count);

	s32 value = 0;

	if (const auto sema = idm::pin<lv2_obj, lv2_sema>(sem_id))
	{
		value = std::max<s32>(0, retro_atomic_load_acquire_int(&sema->val));
	}
	else
	{
		return CELL_ESRCH;
	}

	if (!count)
	{
		return CELL_EFAULT;
	}

	ppu.check_state();

	*count = value;
	return CELL_OK;
}
