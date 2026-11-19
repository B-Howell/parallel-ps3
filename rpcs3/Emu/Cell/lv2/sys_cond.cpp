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
	lv2_waitset_init(&waiters);
}

lv2_cond::lv2_cond(u64 key, u64 name, u32 mtx_id, shared_ptr<lv2_obj> mutex0) noexcept
	: key(key)
	, name(name)
	, mtx_id(mtx_id)
	, mutex(static_cast<lv2_mutex*>(mutex0.get()))
	, _mutex(mutex0)
{
	lv2_waitset_init(&waiters);
}

bool lv2_cond::any_waiter_again()
{
	for (u32 slot = 0; slot < LV2_OWNQ_SLOTS; slot++)
	{
		if (lv2_waitset_has(&waiters, slot))
		{
			if (const auto cpu = thread_in(slot); cpu && cpu->state & cpu_flag::again)
			{
				return true;
			}
		}
	}

	return false;
}

// The rank a condition wakes its waiters by: the mutex's protocol, as
// schedule() had it - the thread's priority, or none (the order they came)
static int cond_rank(void* ctx, unsigned slot)
{
	const auto cpu = static_cast<lv2_cond*>(ctx)->thread_in(slot);
	return cpu ? cpu->get_prio().prio : 0;
}

static lv2_waitset_rank_t cond_ranking(lv2_cond& cond)
{
	return cond.mutex->protocol == SYS_SYNC_FIFO ? nullptr : cond_rank;
}

// A waiter the condition has woken: the mutex's owner now, and woken, or
// queued for it, to be woken by the unlock that hands it over
static void cond_hand_to_mutex(lv2_cond& cond, u32 slot)
{
	ppu_thread* const cpu = ensure(cond.thread_in(slot));

	if (cond.mutex->try_own(*cpu))
	{
		cond.awake(cpu);
	}
}

CellError lv2_cond::on_id_create()
{
	retro_atomic_fetch_add_int(&exists, 1);

	static auto do_it = [](lv2_cond* _this) -> CellError
	{
		if (lv2_obj::check(_this->mutex))
		{
			retro_atomic_fetch_add_int(&_this->mutex->cond_count, 1);
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
		if (lv2_waitset_any(&cond.waiters))
		{
			return CELL_EBUSY;
		}

		retro_atomic_fetch_sub_int(&cond.mutex->cond_count, 1);
		lv2_obj::on_id_destroy(cond, cond.key);
		return {};
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
			if (ppu.state & cpu_flag::suspend)
			{
				// Test if another signal caused the current thread to be suspended, in which case it needs to wait until the thread wakes up (otherwise the signal may cause unexpected results)
				finished = false;
				return;
			}

			if (cond.any_waiter_again())
			{
				ppu.state += cpu_flag::again;
				return;
			}

			// The best waiter taken off in one change, then handed to the mutex
			// TODO: Is EBUSY returned after reqeueing, on sys_cond_destroy?
			if (const int slot = lv2_waitset_claim(&cond.waiters, cond_ranking(cond), &cond); slot >= 0)
			{
				cond_hand_to_mutex(cond, static_cast<u32>(slot));
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
			if (ppu.state & cpu_flag::suspend)
			{
				// Test if another signal caused the current thread to be suspended, in which case it needs to wait until the thread wakes up (otherwise the signal may cause unexpected results)
				finished = false;
				return;
			}

			if (cond.any_waiter_again())
			{
				ppu.state += cpu_flag::again;
				return;
			}

			// All the waiters taken off in one change, then handed to the mutex
			// by priority, as schedule() had it: the first to find it free owns it
			retro_u128_t left = lv2_waitset_claim_all(&cond.waiters);
			cpu_thread* result = nullptr;

			while (left.lo || left.hi)
			{
				int best = -1;
				s32 best_prio = 0;
				int best_tick = 0;

				for (u32 slot = 0; slot < LV2_OWNQ_SLOTS; slot++)
				{
					if (!(slot < 64 ? (left.lo >> slot) & 1 : (left.hi >> (slot - 64)) & 1))
					{
						continue;
					}

					const s32 prio = ensure(cond.thread_in(slot))->get_prio().prio;
					const int tick = retro_atomic_load_acquire_int(&cond.waiters.ticket[slot]);

					if (best < 0 || prio < best_prio || (prio == best_prio && static_cast<int>(static_cast<u32>(tick) - static_cast<u32>(best_tick)) < 0))
					{
						best = static_cast<int>(slot);
						best_prio = prio;
						best_tick = tick;
					}
				}

				(best < 64 ? left.lo : left.hi) &= ~(u64{1} << (best % 64));

				ppu_thread* const cpu = ensure(cond.thread_in(best));

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

	u32 retries = 0;
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

			if (!has_lower_prio_or_is_the_mutex_owner && !cond.mutex->owned_by(ppu))
			{
				if (lv2_ownq_owner(&cond.mutex->q) == static_cast<int>(lv2_mutex::slot_of_id(thread_id)))
				{
					has_lower_prio_or_is_the_mutex_owner = true;
				}
				else if (target_ppu->is_lower_priority_than(ppu))
				{
					has_lower_prio_or_is_the_mutex_owner = true;
				}
			}

			if (ppu.state & cpu_flag::suspend)
			{
				// Test if another signal caused the current thread to be suspended, in which case it needs to wait until the thread wakes up (otherwise the signal may cause unexpected results)
				finished = false;
				return 0;
			}

			const u32 slot = lv2_mutex::slot_of_id(thread_id);
			ppu_thread* const cpu = slot < LV2_OWNQ_SLOTS ? cond.thread_in(slot) : nullptr;

			if (cpu && cpu->id == thread_id && lv2_waitset_has(&cond.waiters, slot))
			{
				if (cpu->state & cpu_flag::again)
				{
					ppu.state += cpu_flag::again;
					return 0;
				}

				// That waiter taken off, if it still waits, then handed to the mutex
				if (lv2_waitset_leave(&cond.waiters, slot))
				{
					cond_hand_to_mutex(cond, slot);
					return 1;
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
			if (has_lower_prio_or_is_the_mutex_owner && retries++ < 8)
			{
				// RPCS3 is not cycle accurate
				// Threads with higher (lower) priority get timing benefits that we cannot provide accurately
				// Give the thread another chance: this one yields to it, in
				// the guest's scheduler, rather than spinning on the host's clock
				lv2_obj::yield(ppu);
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
		if (!ppu.loaded_from_savestate && !cond.mutex->owned_by(ppu))
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

		// The thread joins the condition's waiters before it gives up the
		// mutex: a signal between the two hands it to the mutex it still
		// owns, and it is handed the mutex back in turn. The mutex's next
		// owner is woken on this thread with this thread's sleep, as it was
		const u32 slot = lv2_mutex::slot_of(ppu);
		ppu_thread* next_owner = nullptr;
		s64 count = 0;

		retro_atomic_store_release_ptr(&cond.thread[slot], &ppu);

		if (ppu.loaded_from_savestate)
		{
			if (syscall_state & 1)
			{
				// Mutex sleep
				ensure(!cond.mutex->try_own(ppu));
			}
			else
			{
				ensure(lv2_waitset_join(&cond.waiters, slot));
			}

			count = static_cast<u32>(syscall_state >> 32);
		}
		else
		{
			// Register waiter
			ensure(lv2_waitset_join(&cond.waiters, slot));

			// A mutex waiter to be saved again: the wait is done again after
			for (u32 s = 0; s < LV2_OWNQ_SLOTS; s++)
			{
				if (lv2_ownq_waits(&cond.mutex->q, s))
				{
					if (const auto cpu = cond.mutex->thread_in(s); cpu && cpu->state & cpu_flag::again)
					{
						ensure(lv2_waitset_leave(&cond.waiters, slot));
						ppu.state += cpu_flag::again;
						ppu.cancel_sleep = 0;
						return 0;
					}
				}
			}

			// Unlock the mutex: its recursive count, saved
			count = static_cast<u32>(retro_atomic_exchange_int(&cond.mutex->lock_count, 0));

			const int next = lv2_ownq_unlock(&cond.mutex->q, slot, cond.mutex->ranking(), cond.mutex);
			ensure(next != LV2_OWNQ_NOT_OWNER);

			if (next >= 0)
			{
				cond.mutex->now_owned_by(next);
				next_owner = ensure(cond.mutex->thread_in(next));
			}
			else
			{
				retro_atomic_store_release_int(&cond.mutex->owner_id, 0);
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

			cond_sleep = lv2_waitset_has(&cond->waiters, lv2_mutex::slot_of(ppu));
			mutex_sleep = lv2_ownq_waits(&cond->mutex->q, lv2_mutex::slot_of(ppu));

			if (!cond_sleep && !mutex_sleep)
			{
				break;
			}

			const u64 optional_syscall_state = u32{mutex_sleep} | (u64{static_cast<u32>(cond.ret)} << 32);
			sstate(optional_syscall_state);

			ppu.state += cpu_flag::again;
			return {};
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

				// Off the condition's waiters in one change and onto the mutex's
				// (or its owner), or signalled first: handed to the mutex then,
				// the owner already or queued for it
				const bool owner = [&]
				{
					if (lv2_waitset_leave(&cond->waiters, lv2_mutex::slot_of(ppu)))
					{
						// TODO: Is EBUSY returned after reqeueing, on sys_cond_destroy?
						ppu.gpr[3] = CELL_ETIMEDOUT;

						// Own or requeue
						return cond->mutex->try_own(ppu);
					}

					return cond->mutex->owned_by(ppu);
				}();

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
	ensure(cond->mutex->owned_by(ppu));

	// Restore the recursive value
	retro_atomic_store_release_int(&cond->mutex->lock_count, static_cast<int>(cond.ret));

	return not_an_error(ppu.gpr[3]);
}
