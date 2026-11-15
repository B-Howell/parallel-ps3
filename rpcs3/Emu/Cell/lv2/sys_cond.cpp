#include "stdafx.h"

#include "util/serialization.hpp"
#include "Emu/IdManager.h"
#include "Emu/System.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/timers.hpp"

#include "sys_cond.h"

#include "util/asm.hpp"

LOG_CHANNEL(sys_cond);

lv2_cond::lv2_cond(utils::serial& ar) noexcept
	: key(ar)
	, name(ar)
	, mtx_id(ar)
	, mutex(idm::check_unlocked<lv2_obj, lv2_mutex>(mtx_id))
	, _mutex(idm::get_unlocked<lv2_obj, lv2_mutex>(mtx_id)) // May be nullptr
{
}

lv2_cond::lv2_cond(u64 key, u64 name, u32 mtx_id, shared_ptr<lv2_obj> mutex0) noexcept
	: key(key)
	, name(name)
	, mtx_id(mtx_id)
	, mutex(static_cast<lv2_mutex*>(mutex0.get()))
	, _mutex(mutex0)
{
}

CellError lv2_cond::on_id_create()
{
	retro_atomic_fetch_add_int(&exists, 1);

	static auto do_it = [](lv2_cond* _this) -> CellError
	{
		if (lv2_obj::check(_this->mutex))
		{
			_this->mutex->cond_count++;
			return {};
		}

		// Mutex has been destroyed, cannot create conditional variable
		return CELL_ESRCH;
	};

	if (mutex)
	{
		return do_it(this);
	}

	ensure(!!Emu.DeserialManager());

	Emu.PostponeInitCode([this]()
	{
		if (!mutex)
		{
			_mutex = static_cast<shared_ptr<lv2_obj>>(ensure(idm::get_unlocked<lv2_obj, lv2_mutex>(mtx_id)));
			mutex = static_cast<lv2_mutex*>(_mutex.get());
		}

		// Defer function
		ensure(CellError{} == do_it(this));
	});

	return {};
}

std::function<void(void*)> lv2_cond::load(utils::serial& ar)
{
	return load_func(make_shared<lv2_cond>(stx::exact_t<utils::serial&>(ar)));
}

void lv2_cond::save(utils::serial& ar)
{
	ar(key, name, mtx_id);
}

error_code sys_cond_create(ppu_thread& ppu, vm::ptr<u32> cond_id, u32 mutex_id, vm::ptr<sys_cond_attribute_t> attr)
{
	ppu.state += cpu_flag::wait;

	sys_cond.trace("sys_cond_create(cond_id=*0x%x, mutex_id=0x%x, attr=*0x%x)", cond_id, mutex_id, attr);

	// The ID's own mutex, not a new object in a destroyed one's place (idm::hold)
	auto mutex = idm::hold<lv2_obj, lv2_mutex>(mutex_id);

	if (!mutex)
	{
		return CELL_ESRCH;
	}

	const auto _attr = *attr;

	const u64 ipc_key = lv2_obj::get_key(_attr);

	if (ipc_key)
	{
		sys_cond.warning("sys_cond_create(cond_id=*0x%x, attr=*0x%x): IPC=0x%016x", cond_id, attr, ipc_key);
	}

	if (const auto error = lv2_obj::create<lv2_cond>(_attr.pshared, ipc_key, _attr.flags, [&]
	{
		return make_single<lv2_cond>(
			ipc_key,
			_attr.name_u64,
			mutex_id,
			std::move(mutex));
	}))
	{
		return error;
	}

	ppu.check_state();
	*cond_id = idm::last_id<lv2_cond>();
	return CELL_OK;
}

error_code sys_cond_destroy(ppu_thread& ppu, u32 cond_id)
{
	ppu.state += cpu_flag::wait;

	sys_cond.trace("sys_cond_destroy(cond_id=0x%x)", cond_id);

	const auto cond = idm::withdraw<lv2_obj, lv2_cond>(cond_id, [&](lv2_cond& cond) -> CellError
	{
		return turn_call(&cond.mutex->turns, [&]() -> CellError
		{
			if (atomic_storage<ppu_thread*>::load(cond.sq))
			{
				return CELL_EBUSY;
			}

			cond.mutex->cond_count--;
			lv2_obj::on_id_destroy(cond, cond.key);
			return {};
		});
	});

	if (!cond)
	{
		return CELL_ESRCH;
	}

	if (cond->key)
	{
		sys_cond.warning("sys_cond_destroy(cond_id=0x%x): IPC=0x%016x", cond_id, cond->key);
	}

	if (cond.ret)
	{
		return cond.ret;
	}

	return CELL_OK;
}

error_code sys_cond_signal(ppu_thread& ppu, u32 cond_id)
{
	ppu.state += cpu_flag::wait;

	sys_cond.trace("sys_cond_signal(cond_id=0x%x)", cond_id);

	while (true)
	{
		if (ppu.test_stopped())
		{
			ppu.state += cpu_flag::again;
			return {};
		}

		bool finished = true;

		ppu.state += cpu_flag::wait;

		// Found with no lock over the IDs: waking threads makes none wait on the condition, which is all sys_cond_destroy asks about
		const auto cond = idm::get_held<lv2_obj, lv2_cond>(cond_id, [&, notify = lv2_obj::notify_all_t()](lv2_cond& cond)
		{
			if (atomic_storage<ppu_thread*>::load(cond.sq))
			{
				// In the mutex's turn: the condition's queue and the mutex's change together
				turn_call(&cond.mutex->turns, [&]
				{
					if (ppu.state & cpu_flag::suspend)
					{
						// Test if another signal caused the current thread to be suspended, in which case it needs to wait until the thread wakes up (otherwise the signal may cause unexpected results)
						finished = false;
						return;
					}

					if (const auto cpu = cond.schedule<ppu_thread>(cond.sq, cond.mutex->protocol))
					{
						if (static_cast<ppu_thread*>(cpu)->state & cpu_flag::again)
						{
							ppu.state += cpu_flag::again;
							return;
						}

						// TODO: Is EBUSY returned after reqeueing, on sys_cond_destroy?

						if (cond.mutex->try_own(*cpu))
						{
							cond.awake(cpu);
						}
					}
				});
			}
			else
			{
				// A signal in the mutex's turn meanwhile: waited out
				turn_call(&cond.mutex->turns, [] {});

				if (ppu.state & cpu_flag::suspend)
				{
					finished = false;
				}
			}
		});

		if (!finished)
		{
			continue;
		}

		if (!cond)
		{
			return CELL_ESRCH;
		}

		return CELL_OK;
	}
}

error_code sys_cond_signal_all(ppu_thread& ppu, u32 cond_id)
{
	ppu.state += cpu_flag::wait;

	sys_cond.trace("sys_cond_signal_all(cond_id=0x%x)", cond_id);

	while (true)
	{
		if (ppu.test_stopped())
		{
			ppu.state += cpu_flag::again;
			return {};
		}

		bool finished = true;

		ppu.state += cpu_flag::wait;

		// Found with no lock over the IDs: waking threads makes none wait on the condition, which is all sys_cond_destroy asks about
		const auto cond = idm::get_held<lv2_obj, lv2_cond>(cond_id, [&, notify = lv2_obj::notify_all_t()](lv2_cond& cond)
		{
			if (atomic_storage<ppu_thread*>::load(cond.sq))
			{
				// In the mutex's turn: the condition's queue and the mutex's change together
				turn_call(&cond.mutex->turns, [&]
				{
					if (ppu.state & cpu_flag::suspend)
					{
						// Test if another signal caused the current thread to be suspended, in which case it needs to wait until the thread wakes up (otherwise the signal may cause unexpected results)
						finished = false;
						return;
					}

					for (auto cpu = +cond.sq; cpu; cpu = cpu->next_cpu)
					{
						if (cpu->state & cpu_flag::again)
						{
							ppu.state += cpu_flag::again;
							return;
						}
					}

					cpu_thread* result = nullptr;
					auto sq = cond.sq;
					atomic_storage<ppu_thread*>::release(cond.sq, nullptr);

					while (const auto cpu = cond.schedule<ppu_thread>(sq, SYS_SYNC_PRIORITY))
					{
						if (cond.mutex->try_own(*cpu))
						{
							ensure(!std::exchange(result, cpu));
						}
					}

					if (result)
					{
						cond.awake(result);
					}
				});
			}
			else
			{
				// A signal in the mutex's turn meanwhile: waited out
				turn_call(&cond.mutex->turns, [] {});

				if (ppu.state & cpu_flag::suspend)
				{
					finished = false;
				}
			}
		});

		if (!finished)
		{
			continue;
		}

		if (!cond)
		{
			return CELL_ESRCH;
		}

		return CELL_OK;
	}
}

error_code sys_cond_signal_to(ppu_thread& ppu, u32 cond_id, u32 thread_id)
{
	ppu.state += cpu_flag::wait;

	sys_cond.trace("sys_cond_signal_to(cond_id=0x%x, thread_id=0x%x)", cond_id, thread_id);

	const u64 start_time = get_system_time();
	bool has_lower_prio_or_is_the_mutex_owner = false;

	while (true)
	{
		if (ppu.test_stopped())
		{
			ppu.state += cpu_flag::again;
			return {};
		}

		bool finished = true;

		ppu.state += cpu_flag::wait;

		// Found with no lock over the IDs: waking threads makes none wait on the condition, which is all sys_cond_destroy asks about
		const auto cond = idm::get_held<lv2_obj, lv2_cond>(cond_id, [&, notify = lv2_obj::notify_all_t()](lv2_cond& cond)
		{
			const auto target_ppu = idm::check_unlocked<named_thread<ppu_thread>>(thread_id);

			if (!target_ppu)
			{
				return -1;
			}

			if (!has_lower_prio_or_is_the_mutex_owner && cond.mutex->control_owner() != ppu.id)
			{
				if (cond.mutex->control_owner() == thread_id)
				{
					has_lower_prio_or_is_the_mutex_owner = true;
				}
				else if (target_ppu->is_lower_priority_than(ppu))
				{
					has_lower_prio_or_is_the_mutex_owner = true;
				}
			}

			if (atomic_storage<ppu_thread*>::load(cond.sq))
			{
				// In the mutex's turn: the condition's queue and the mutex's change together
				return turn_call(&cond.mutex->turns, [&]() -> int
				{
					if (ppu.state & cpu_flag::suspend)
					{
						// Test if another signal caused the current thread to be suspended, in which case it needs to wait until the thread wakes up (otherwise the signal may cause unexpected results)
						finished = false;
						return 0;
					}

					for (auto cpu = +cond.sq; cpu; cpu = cpu->next_cpu)
					{
						if (cpu->id == thread_id)
						{
							if (static_cast<ppu_thread*>(cpu)->state & cpu_flag::again)
							{
								ppu.state += cpu_flag::again;
								return 0;
							}

							ensure(cond.unqueue(cond.sq, cpu));

							if (cond.mutex->try_own(*cpu))
							{
								cond.awake(cpu);
							}

							return 1;
						}
					}

					return 0;
				});
			}
			else
			{
				// A signal in the mutex's turn meanwhile: waited out
				turn_call(&cond.mutex->turns, [] {});

				if (ppu.state & cpu_flag::suspend)
				{
					finished = false;
					return 0;
				}
			}

			return 0;
		});

		if (!finished)
		{
			continue;
		}

		if (!cond || cond.ret == -1)
		{
			return CELL_ESRCH;
		}

		if (!cond.ret)
		{
			if (has_lower_prio_or_is_the_mutex_owner && get_system_time() - start_time < 50)
			{
				// RPCS3 is not cycle accurate
				// Threads with higher (lower) priority get timing benefits that we cannot provide accurately
				// Give the thread another chance
				busy_wait(5000);
				continue;
			}

			return { CELL_EPERM, thread_id };
		}

		return CELL_OK;
	}
}

error_code sys_cond_wait(ppu_thread& ppu, u32 cond_id, u64 timeout)
{
	ppu.state += cpu_flag::wait;

	sys_cond.trace("sys_cond_wait(cond_id=0x%x, timeout=%lld)", cond_id, timeout);

	// Further function result
	ppu.gpr[3] = CELL_OK;

	auto& sstate = *ppu.optional_savestate_state;

	// Woken, and the mutex's owner, before its sleep began: no wait follows
	bool woken_early = false;

	// Found with no lock over the IDs. The thread makes itself a waiter, so a
	// destroy that is looking for waiters waits for it to be in the queue
	// (idm::get_entered).
	const auto cond = idm::get_entered<lv2_obj, lv2_cond>(cond_id, [&, notify = lv2_obj::notify_all_t()](lv2_cond& cond) -> s64
	{
		if (!ppu.loaded_from_savestate && cond.mutex->control_owner() != ppu.id)
		{
			return -1;
		}

		lv2_obj::prepare_for_sleep(ppu);

		const u64 syscall_state = sstate.try_read<u64>().second;
		sstate.clear();

		// The thread is a waiter once it is on a queue, and may be woken - given
		// the mutex - before it has gone to sleep: that sleep is then cancelled
		// (lv2_obj::sleep gives false), as sys_mutex_lock has it
		ppu.cancel_sleep = 1;

		// The queues change in the mutex's turn. The mutex's next owner is
		// woken on this thread with this thread's sleep, after the turn, as
		// it was: the threads to wake are this thread's own list
		ppu_thread* next_owner = nullptr;
		s64 count = 0;

		if (ppu.loaded_from_savestate)
		{
			turn_call(&cond.mutex->turns, [&]
			{
				if (syscall_state & 1)
				{
					// Mutex sleep
					ensure(!cond.mutex->try_own(ppu));
				}
				else
				{
					lv2_obj::emplace(cond.sq, &ppu);
				}
			});

			count = static_cast<u32>(syscall_state >> 32);
		}
		else
		{
			// Unlocking the mutex: its recursive count, saved
			count = turn_call(&cond.mutex->turns, [&]() -> s64
			{
				// Register waiter
				lv2_obj::emplace(cond.sq, &ppu);

				// Unlock the mutex
				const u32 locks = static_cast<u32>(retro_atomic_exchange_int(&cond.mutex->lock_count, 0));

				if (const auto cpu = cond.mutex->reown<ppu_thread>())
				{
					if (cpu->state & cpu_flag::again)
					{
						ensure(cond.unqueue(cond.sq, &ppu));
						ppu.state += cpu_flag::again;
						return 0;
					}

					next_owner = cpu;
				}

				return locks;
			});

			if (ppu.state & cpu_flag::again)
			{
				ppu.cancel_sleep = 0;
				return 0;
			}
		}

		if (next_owner)
		{
			cond.mutex->append(next_owner);
		}

		// Sleep current thread and schedule mutex waiter
		if (!cond.sleep(ppu, timeout))
		{
			// Given the mutex before it went to sleep: the owner already
			woken_early = true;
		}

		if (ppu.cancel_sleep != 1)
		{
			notify.cleanup();
		}

		ppu.cancel_sleep = 0;

		// Save the recursive value
		return count;
	});

	if (!cond)
	{
		return CELL_ESRCH;
	}

	if (ppu.state & cpu_flag::again)
	{
		return {};
	}

	if (cond.ret < 0)
	{
		return CELL_EPERM;
	}

	while (auto state = woken_early ? bs_t<cpu_flag>{} : +ppu.state)
	{
		if (state & cpu_flag::signal && ppu.state.test_and_reset(cpu_flag::signal))
		{
			break;
		}

		if (is_stopped(state))
		{
			bool mutex_sleep = false;
			bool cond_sleep = false;

			turn_call(&cond->mutex->turns, [&]
			{
				for (auto cpu = atomic_storage<ppu_thread*>::load(cond->sq); cpu; cpu = cpu->next_cpu)
				{
					if (cpu == &ppu)
					{
						cond_sleep = true;
						break;
					}
				}

				for (auto cpu = cond->mutex->control_sq(); cpu; cpu = cpu->next_cpu)
				{
					if (cpu == &ppu)
					{
						mutex_sleep = true;
						break;
					}
				}
			});

			if (!cond_sleep && !mutex_sleep)
			{
				break;
			}

			const u64 optional_syscall_state = u32{mutex_sleep} | (u64{static_cast<u32>(cond.ret)} << 32);
			sstate(optional_syscall_state);

			ppu.state += cpu_flag::again;
			return {};
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
				const u64 start_time = ppu.start_time;

				// Wait for rescheduling
				if (ppu.check_state())
				{
					continue;
				}

				ppu.state += cpu_flag::wait;

				// May be given the mutex before the sleep below: cancelled then
				ppu.cancel_sleep = 1;

				// In the mutex's turn: off the condition's queue and onto the
				// mutex's (or its owner), or found the owner already
				const bool owner = turn_call(&cond->mutex->turns, [&]
				{
					// Try to cancel the waiting
					if (cond->unqueue(cond->sq, &ppu))
					{
						// TODO: Is EBUSY returned after reqeueing, on sys_cond_destroy?
						ppu.gpr[3] = CELL_ETIMEDOUT;

						// Own or requeue
						return cond->mutex->try_own(ppu);
					}

					return cond->mutex->control_owner() == ppu.id;
				});

				if (owner)
				{
					ppu.cancel_sleep = 0;
					break;
				}

				const bool slept = cond->mutex->sleep(ppu);
				ppu.cancel_sleep = 0;

				if (!slept)
				{
					// Given the mutex before it went to sleep
					break;
				}

				ppu.start_time = start_time; // Restore start time because awake has been called
				timeout = 0;
				continue;
			}
		}
		else
		{
			ppu.state.wait(state);
		}
	}

	// Verify ownership
	ensure(cond->mutex->control_owner() == ppu.id);

	// Restore the recursive value
	retro_atomic_store_release_int(&cond->mutex->lock_count, static_cast<int>(cond.ret));

	return not_an_error(ppu.gpr[3]);
}
