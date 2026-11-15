#include "stdafx.h"
#include "sys_lwcond.h"

#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
#include "sys_lwmutex.h"

#include "util/asm.hpp"

LOG_CHANNEL(sys_lwcond);

lv2_lwcond::lv2_lwcond(utils::serial& ar)
	: name(ar.pop<be_t<u64>>())
	, lwid(ar)
	, protocol(ar)
	, control(ar.pop<decltype(control)>())
{
}

void lv2_lwcond::save(utils::serial& ar)
{
	USING_SERIALIZATION_VERSION(lv2_sync);
	ar(name, lwid, protocol, control);
}

error_code _sys_lwcond_create(ppu_thread& ppu, vm::ptr<u32> lwcond_id, u32 lwmutex_id, vm::ptr<sys_lwcond_t> control, u64 name)
{
	ppu.state += cpu_flag::wait;

	sys_lwcond.trace(u8"_sys_lwcond_create(lwcond_id=*0x%x, lwmutex_id=0x%x, control=*0x%x, name=0x%llx (“%s”))", lwcond_id, lwmutex_id, control, name, lv2_obj::name_64{std::bit_cast<be_t<u64>>(name)});

	u32 protocol;

	// Extract protocol from lwmutex
	if (const auto mutex = idm::pin<lv2_obj, lv2_lwmutex>(lwmutex_id))
	{
		protocol = mutex->protocol;
	}
	else
	{
		return CELL_ESRCH;
	}

	if (protocol == SYS_SYNC_RETRY)
	{
		// Lwcond can't have SYS_SYNC_RETRY protocol
		protocol = SYS_SYNC_PRIORITY;
	}

	if (const u32 id = idm::make<lv2_obj, lv2_lwcond>(name, lwmutex_id, protocol, control))
	{
		ppu.check_state();
		*lwcond_id = id;
		return CELL_OK;
	}

	return CELL_EAGAIN;
}

error_code _sys_lwcond_destroy(ppu_thread& ppu, u32 lwcond_id)
{
	ppu.state += cpu_flag::wait;

	sys_lwcond.trace("_sys_lwcond_destroy(lwcond_id=0x%x)", lwcond_id);

	shared_ptr<lv2_lwcond> _cond;

	while (true)
	{
		s32 old_val = 0;

		auto [ptr, ret] = idm::withdraw<lv2_obj, lv2_lwcond>(lwcond_id, [&](lv2_lwcond& cond) -> CellError
		{
			// Ignore check on first iteration
			if (_cond && std::addressof(cond) != _cond.get())
			{
				// Other thread has destroyed the lwcond earlier
				return CELL_ESRCH;
			}

			const CellError found = turn_call(&cond.turns, [&]() -> CellError
			{
				if (atomic_storage<ppu_thread*>::load(cond.sq))
				{
					return CELL_EBUSY;
				}

				old_val = cond.lwmutex_waiters.or_fetch(smin);

				if (old_val != smin)
				{
					// Repeat loop: there are lwmutex waiters inside _sys_lwcond_queue_wait
					return CELL_EAGAIN;
				}

				return {};
			});

			if (found == CELL_EAGAIN)
			{
				// De-schedule if waiters were found: this thread's own, out of the turn
				lv2_obj::sleep(ppu);
			}

			return found;
		});

		if (!ptr)
		{
			return CELL_ESRCH;
		}

		if (ret)
		{
			if (ret != CELL_EAGAIN)
			{
				return ret;
			}
		}
		else
		{
			break;
		}

		_cond = std::move(ptr);

		// Wait for all lwcond waiters to quit
		while (old_val + 0u > 1u << 31)
		{
			thread_ctrl::wait_on(_cond->lwmutex_waiters, old_val);

			if (ppu.is_stopped())
			{
				ppu.state += cpu_flag::again;
				return {};
			}

			old_val = _cond->lwmutex_waiters;
		}

		// Wake up from sleep
		ppu.check_state();
	}

	return CELL_OK;
}

error_code _sys_lwcond_signal(ppu_thread& ppu, u32 lwcond_id, u32 lwmutex_id, u64 ppu_thread_id, u32 mode)
{
	ppu.state += cpu_flag::wait;

	sys_lwcond.trace("_sys_lwcond_signal(lwcond_id=0x%x, lwmutex_id=0x%x, ppu_thread_id=0x%llx, mode=%d)", lwcond_id, lwmutex_id, ppu_thread_id, mode);

	// Mode 1: lwmutex was initially owned by the calling thread
	// Mode 2: lwmutex was not owned by the calling thread and waiter hasn't been increased
	// Mode 3: lwmutex was forcefully owned by the calling thread

	if (mode < 1 || mode > 3)
	{
		fmt::throw_exception("Unknown mode (%d)", mode);
	}

	while (true)
	{
		if (ppu.test_stopped())
		{
			ppu.state += cpu_flag::again;
			return {};
		}

		bool finished = true;

		ppu.state += cpu_flag::wait;

		// Found with no lock over the IDs. Waking a thread makes none wait on
		// the lwcond, which is what _sys_lwcond_destroy asks about, so the
		// lwcond is only kept (idm::get_held). The thread and the lwmutex
		// the call names were looked up as bare pointers, which the lock
		// over all IDs kept alive; they are kept with references now. And
		// the woken thread may be put in the lwmutex's queue, which is what
		// _sys_lwmutex_destroy asks about: the lwmutex is entered
		// (idm::enter), before any of the objects' own locks is taken - a
		// thread that waits for a destroy to decide must not hold a lock
		// that a thread the destroy waits for needs.
		const auto cond = idm::get_held<lv2_obj, lv2_lwcond>(lwcond_id, [&, notify = lv2_obj::notify_all_t()](lv2_lwcond& cond) -> int
		{
			stx::shared_ptr<named_thread<ppu_thread>> target;
			ppu_thread* cpu = nullptr;

			if (ppu_thread_id != u32{umax})
			{
				target = idm::hold<named_thread<ppu_thread>>(static_cast<u32>(ppu_thread_id));
				cpu = target.get();

				if (!cpu)
				{
					return -1;
				}
			}

			stx::shared_ptr<lv2_lwmutex> lwmutex;
			lv2_lwmutex* mutex = nullptr;

			if (mode != 2)
			{
				lwmutex = idm::hold<lv2_obj, lv2_lwmutex>(lwmutex_id);
				mutex = lwmutex.get();

				if (!mutex)
				{
					return -1;
				}
			}

			const auto mutex_entered = mutex ? idm::enter<lv2_obj, lv2_lwmutex>(lwmutex_id, mutex) : idm::entered{};

			if (mutex && !mutex_entered)
			{
				return -1;
			}

			if (atomic_storage<ppu_thread*>::load(cond.sq))
			{
				// In the lwcond's turn
				return turn_call(&cond.turns, [&]() -> int
				{
					if (ppu.state & cpu_flag::suspend)
					{
						// Test if another signal caused the current thread to be suspended, in which case it needs to wait until the thread wakes up (otherwise the signal may cause unexpected results)
						finished = false;
						return 0;
					}

					if (cpu)
					{
						if (static_cast<ppu_thread*>(cpu)->state & cpu_flag::again)
						{
							ppu.state += cpu_flag::again;
							return 0;
						}
					}

					auto result = cpu ? cond.unqueue(cond.sq, cpu) :
						cond.schedule<ppu_thread>(cond.sq, cond.protocol);

					if (result)
					{
						if (static_cast<ppu_thread*>(result)->state & cpu_flag::again)
						{
							ppu.state += cpu_flag::again;
							return 0;
						}

						if (mode == 2)
						{
							static_cast<ppu_thread*>(result)->gpr[3] = CELL_EBUSY;
						}
						else if (mode == 3 && mutex->load_sq()) [[unlikely]]
						{
							// In the lwmutex's turn, inside the lwcond's
							const bool again = turn_call(&mutex->turns, [&]
							{
								// Respect ordering of the sleep queue
								mutex->try_own(result, true);

								// There is one: the thread just queued, if no other
								auto result2 = ensure(mutex->reown<ppu_thread>());

								if (result2->state & cpu_flag::again)
								{
									return true;
								}

								if (result2 != result)
								{
									cond.awake(result2);
									result = nullptr;
								}

								return false;
							});

							if (again)
							{
								ppu.state += cpu_flag::again;
								return 0;
							}
						}
						else if (mode == 1)
						{
							mutex->try_own(result, true);
							result = nullptr;
						}

						if (result)
						{
							cond.awake(result);
						}

						return 1;
					}

					return 0;
				});
			}
			else
			{
				// A signal in the lwcond's turn meanwhile: waited out
				turn_call(&cond.turns, [] {});

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
			if (ppu_thread_id == u32{umax})
			{
				if (mode == 3)
				{
					return not_an_error(CELL_ENOENT);
				}
				else if (mode == 2)
				{
					return CELL_OK;
				}

				return not_an_error(CELL_EPERM);
			}

			return { CELL_EPERM, ppu_thread_id };
		}

		return CELL_OK;
	}
}

error_code _sys_lwcond_signal_all(ppu_thread& ppu, u32 lwcond_id, u32 lwmutex_id, u32 mode)
{
	ppu.state += cpu_flag::wait;

	sys_lwcond.trace("_sys_lwcond_signal_all(lwcond_id=0x%x, lwmutex_id=0x%x, mode=%d)", lwcond_id, lwmutex_id, mode);

	// Mode 1: lwmutex was initially owned by the calling thread
	// Mode 2: lwmutex was not owned by the calling thread and waiter hasn't been increased

	if (mode < 1 || mode > 2)
	{
		fmt::throw_exception("Unknown mode (%d)", mode);
	}

	while (true)
	{
		if (ppu.test_stopped())
		{
			ppu.state += cpu_flag::again;
			return {};
		}

		bool finished = true;

		ppu.state += cpu_flag::wait;

		// As _sys_lwcond_signal
		const auto cond = idm::get_held<lv2_obj, lv2_lwcond>(lwcond_id, [&, notify = lv2_obj::notify_all_t()](lv2_lwcond& cond) -> int
		{
			stx::shared_ptr<lv2_lwmutex> lwmutex;
			lv2_lwmutex* mutex{};

			if (mode != 2)
			{
				lwmutex = idm::hold<lv2_obj, lv2_lwmutex>(lwmutex_id);
				mutex = lwmutex.get();

				if (!mutex)
				{
					return -1;
				}
			}

			const auto mutex_entered = mutex ? idm::enter<lv2_obj, lv2_lwmutex>(lwmutex_id, mutex) : idm::entered{};

			if (mutex && !mutex_entered)
			{
				return -1;
			}

			if (atomic_storage<ppu_thread*>::load(cond.sq))
			{
				// In the lwcond's turn
				return turn_call(&cond.turns, [&]() -> int
				{
					if (ppu.state & cpu_flag::suspend)
					{
						// Test if another signal caused the current thread to be suspended, in which case it needs to wait until the thread wakes up (otherwise the signal may cause unexpected results)
						finished = false;
						return 0;
					}

					u32 result = 0;

					for (auto cpu = +cond.sq; cpu; cpu = cpu->next_cpu)
					{
						if (cpu->state & cpu_flag::again)
						{
							ppu.state += cpu_flag::again;
							return 0;
						}
					}

					auto sq = cond.sq;
					atomic_storage<ppu_thread*>::release(cond.sq, nullptr);

					while (const auto cpu = cond.schedule<ppu_thread>(sq, cond.protocol))
					{
						if (mode == 2)
						{
							static_cast<ppu_thread*>(cpu)->gpr[3] = CELL_EBUSY;
						}

						if (mode == 1)
						{
							mutex->try_own(cpu, true);
						}
						else
						{
							lv2_obj::append(cpu);
						}

						result++;
					}

					if (result && mode == 2)
					{
						lv2_obj::awake_all();
					}

					return result;
				});
			}
			else
			{
				// A signal in the lwcond's turn meanwhile: waited out
				turn_call(&cond.turns, [] {});

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

		if (mode == 1)
		{
			// Mode 1: return the amount of threads (TODO)
			return not_an_error(cond.ret);
		}

		return CELL_OK;
	}
}

error_code _sys_lwcond_queue_wait(ppu_thread& ppu, u32 lwcond_id, u32 lwmutex_id, u64 timeout)
{
	ppu.state += cpu_flag::wait;

	sys_lwcond.trace("_sys_lwcond_queue_wait(lwcond_id=0x%x, lwmutex_id=0x%x, timeout=0x%llx)", lwcond_id, lwmutex_id, timeout);

	ppu.gpr[3] = CELL_OK;

	shared_ptr<lv2_lwmutex> mutex;

	auto& sstate = *ppu.optional_savestate_state;

	// Woken before its sleep began: no wait follows
	bool woken_early = false;

	// Found with no lock over the IDs. The thread makes itself a waiter of
	// the lwcond, which _sys_lwcond_destroy asks about (idm::get_entered),
	// and counts itself among the lwmutex's lwcond waiters, which
	// _sys_lwmutex_destroy asks about: the lwmutex is entered too, before
	// either object's own lock is taken (see _sys_lwcond_signal).
	const auto cond = idm::get_entered<lv2_obj, lv2_lwcond>(lwcond_id, [&, notify = lv2_obj::notify_all_t()](lv2_lwcond& cond)
	{
		mutex = idm::hold<lv2_obj, lv2_lwmutex>(lwmutex_id);

		if (!mutex)
		{
			return;
		}

		const auto mutex_entered = idm::enter<lv2_obj, lv2_lwmutex>(lwmutex_id, mutex.get());

		if (!mutex_entered)
		{
			mutex = null_ptr;
			return;
		}

		// Increment lwmutex's lwcond's waiters count
		mutex->lwcond_waiters++;

		lv2_obj::prepare_for_sleep(ppu);

		const bool mutex_sleep = sstate.try_read<bool>().second;
		sstate.clear();

		// A waiter once on a queue, and may be woken before it has gone to
		// sleep: that sleep is then cancelled (lv2_obj::sleep gives false)
		ppu.cancel_sleep = 1;

		// The queues change in the lwcond's turn, the lwmutex's inside it. The
		// lwmutex's next owner is woken on this thread with this thread's
		// sleep, after the turn, as it was: the threads to wake are this
		// thread's own list
		ppu_thread* next_owner = nullptr;

		const bool again = turn_call(&cond.turns, [&]
		{
			cond.lwmutex_waiters++;

			if (mutex_sleep)
			{
				// Special: loading state from the point of waiting on lwmutex sleep queue
				mutex->try_own(&ppu, true);
			}
			else
			{
				// Add a waiter
				lv2_obj::emplace(cond.sq, &ppu);
			}

			if (!ppu.loaded_from_savestate && !mutex->try_unlock(false))
			{
				// Process lwmutex sleep queue
				const auto cpu = turn_call(&mutex->turns, [&] { return mutex->reown<ppu_thread>(); });

				if (cpu)
				{
					if (static_cast<ppu_thread*>(cpu)->state & cpu_flag::again)
					{
						ensure(cond.unqueue(cond.sq, &ppu));
						cond.lwmutex_waiters--;
						mutex->lwcond_waiters--;
						return true;
					}

					next_owner = cpu;
				}
			}

			return false;
		});

		if (again)
		{
			ppu.cancel_sleep = 0;
			ppu.state += cpu_flag::again;
			return;
		}

		if (next_owner)
		{
			// Put the current thread to sleep and schedule lwmutex waiter together
			cond.append(next_owner);
		}

		if (!cond.sleep(ppu, timeout))
		{
			// Woken before it went to sleep: gpr[3] is what the waker gave it
			woken_early = true;
		}

		if (ppu.cancel_sleep != 1)
		{
			notify.cleanup();
		}

		ppu.cancel_sleep = 0;
	});

	if (!cond || !mutex)
	{
		return { CELL_ESRCH, fmt::format("lwmutex_id: 0x%x, lwcond_id: 0x%x", lwmutex_id, lwcond_id) };
	}

	if (ppu.state & cpu_flag::again)
	{
		return CELL_OK;
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

			// In the lwcond's turn and the lwmutex's inside it, both queues still
			turn_call(&cond->turns, [&]
			{
				turn_call(&mutex->turns, [&]
				{
					for (auto cpu = mutex->load_sq(); cpu; cpu = cpu->next_cpu)
					{
						if (cpu == &ppu)
						{
							mutex_sleep = true;
							break;
						}
					}

					for (auto cpu = atomic_storage<ppu_thread*>::load(cond->sq); cpu; cpu = cpu->next_cpu)
					{
						if (cpu == &ppu)
						{
							cond_sleep = true;
							break;
						}
					}
				});
			});

			if (!cond_sleep && !mutex_sleep)
			{
				break;
			}

			sstate(mutex_sleep);
			ppu.state += cpu_flag::again;
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

				// Off the lwcond's queue in its turn, or off the lwmutex's in that
				// one's, inside it - or given the lwmutex by an unlock already
				bool success = false;

				const bool off_cond = turn_call(&cond->turns, [&]
				{
					if (cond->unqueue(cond->sq, &ppu))
					{
						return true;
					}

					turn_call(&mutex->turns, [&]
					{
						mutex->change_control([&](lv2_lwmutex::control_data_t& data)
						{
							success = false;

							ppu_thread* sq = static_cast<ppu_thread*>(data.sq);

							const bool retval = &ppu == sq;

							if (!mutex->unqueue<false>(sq, &ppu))
							{
								return false;
							}

							success = true;

							if (!retval)
							{
								return false;
							}

							data.sq = sq;
							return true;
						});
					});

					return false;
				});

				if (off_cond)
				{
					ppu.gpr[3] = CELL_ETIMEDOUT;
					break;
				}

				if (success)
				{
					ppu.next_cpu = nullptr;
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

	if (--mutex->lwcond_waiters == smin)
	{
		// Notify the thread destroying lwmutex on last waiter
		mutex->lwcond_waiters.notify_all();
	}

	if (--cond->lwmutex_waiters == smin)
	{
		// Notify the thread destroying lwcond on last waiter
		cond->lwmutex_waiters.notify_all();
	}

	// Return cause
	return not_an_error(ppu.gpr[3]);
}
