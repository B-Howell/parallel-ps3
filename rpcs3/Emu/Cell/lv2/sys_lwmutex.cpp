#include "stdafx.h"
#include "sys_lwmutex.h"

#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"

#include "util/asm.hpp"

LOG_CHANNEL(sys_lwmutex);

lv2_lwmutex::lv2_lwmutex(utils::serial& ar)
	: protocol(ar)
	, control(ar.pop<decltype(control)>())
	, name(ar.pop<be_t<u64>>())
{
	s32 signaled = 0;
	ar(signaled);
	lv2_lwq_init(&q, static_cast<u32>(signaled));
}

void lv2_lwmutex::save(utils::serial& ar)
{
	ar(protocol, control, name, static_cast<s32>(lv2_lwq_signal(&q)));
}

static_assert(ppu_thread::id_count <= LV2_LWQ_SLOTS, "a PPU thread's slot in an lwmutex's word is its ID's index");

u32 lv2_lwmutex::slot_of(const ppu_thread& ppu)
{
	using traits = id_manager::id_traits<named_thread<ppu_thread>>;
	return id_manager::get_index(ppu.id, traits::base, traits::step, traits::count, traits::invl_range);
}

int lv2_lwmutex::rank(void* ctx, unsigned slot)
{
	const auto ppu = static_cast<lv2_lwmutex*>(ctx)->thread_in(slot);
	return ppu ? ppu->prio.load().prio : 0;
}

s32 lv2_lwmutex::try_own(ppu_thread* cpu)
{
	const u32 slot = slot_of(*cpu);
	retro_atomic_store_release_ptr(&thread[slot], cpu);

	u32 signal = 0;
	const int got = lv2_lwq_lock(&q, slot, &signal);
	ensure(got != LV2_LWQ_BAD);

	if (got == LV2_LWQ_OWNED)
	{
		return static_cast<s32>(signal);
	}

	const bool notify = lwcond_waiters.fetch_op([](s32& val)
	{
		if (val + 0u <= 1u << 31)
		{
			// Value was either positive or INT32_MIN
			return false;
		}

		// lwmutex was set to be destroyed, but there are lwcond waiters
		// Turn off the "lwcond_waiters notification" bit as we are adding an lwmutex waiter
		val &= 0x7fff'ffff;
		return true;
	}).second;

	if (notify)
	{
		// Notify lwmutex destroyer (may cause EBUSY to be returned for it)
		lwcond_waiters.notify_all();
	}

	return 0;
}

bool lv2_lwmutex::resaving()
{
	if (!lv2_lwq_any_waiter(&q))
	{
		return false;
	}

	for (u32 slot = 0; slot < LV2_LWQ_SLOTS; slot++)
	{
		if (lv2_lwq_waits(&q, slot))
		{
			const auto cpu = thread_in(slot);

			if (cpu && cpu->state & cpu_flag::again)
			{
				return true;
			}
		}
	}

	return false;
}

error_code _sys_lwmutex_create(ppu_thread& ppu, vm::ptr<u32> lwmutex_id, u32 protocol, vm::ptr<sys_lwmutex_t> control, s32 has_name, u64 name)
{
	ppu.state += cpu_flag::wait;

	sys_lwmutex.trace(u8"_sys_lwmutex_create(lwmutex_id=*0x%x, protocol=0x%x, control=*0x%x, has_name=0x%x, name=0x%llx (“%s”))", lwmutex_id, protocol, control, has_name, name, lv2_obj::name_64{std::bit_cast<be_t<u64>>(name)});

	if (protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_RETRY && protocol != SYS_SYNC_PRIORITY)
	{
		sys_lwmutex.error("_sys_lwmutex_create(): unknown protocol (0x%x)", protocol);
		return CELL_EINVAL;
	}

	if (!(has_name < 0))
	{
		name = 0;
	}

	if (const u32 id = idm::make<lv2_obj, lv2_lwmutex>(protocol, control, name))
	{
		ppu.check_state();
		*lwmutex_id = id;
		return CELL_OK;
	}

	return CELL_EAGAIN;
}

error_code _sys_lwmutex_destroy(ppu_thread& ppu, u32 lwmutex_id)
{
	ppu.state += cpu_flag::wait;

	sys_lwmutex.trace("_sys_lwmutex_destroy(lwmutex_id=0x%x)", lwmutex_id);

	shared_ptr<lv2_lwmutex> _mutex;

	while (true)
	{
		s32 old_val = 0;

		auto [ptr, ret] = idm::withdraw<lv2_obj, lv2_lwmutex>(lwmutex_id, [&](lv2_lwmutex& mutex) -> CellError
		{
			// Ignore check on first iteration
			if (_mutex && std::addressof(mutex) != _mutex.get())
			{
				// Other thread has destroyed the lwmutex earlier
				return CELL_ESRCH;
			}

			// A thread that sleeps on it got in before this (idm::enter), and is
			// in its word
			if (lv2_lwq_any_waiter(&mutex.q))
			{
				return CELL_EBUSY;
			}

			old_val = mutex.lwcond_waiters.or_fetch(smin);

			if (old_val != smin)
			{
				// Repeat loop: there are lwcond waiters. Descheduled meanwhile
				lv2_obj::sleep(ppu);
				return CELL_EAGAIN;
			}

			return {};
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

		_mutex = std::move(ptr);

		// Wait for all lwcond waiters to quit
		while (old_val + 0u > 1u << 31)
		{
			thread_ctrl::wait_on(_mutex->lwcond_waiters, old_val);

			if (ppu.is_stopped())
			{
				ppu.state += cpu_flag::again;
				return {};
			}

			old_val = _mutex->lwcond_waiters;
		}

		// Wake up from sleep
		ppu.check_state();
	}

	return CELL_OK;
}

error_code _sys_lwmutex_lock(ppu_thread& ppu, u32 lwmutex_id, u64 timeout)
{
	ppu.state += cpu_flag::wait;

	sys_lwmutex.trace("_sys_lwmutex_lock(lwmutex_id=0x%x, timeout=0x%llx)", lwmutex_id, timeout);

	ppu.gpr[3] = CELL_OK;

	const auto take_signal = [&](lv2_lwmutex& mutex) -> bool
	{
		if (const s32 signal = static_cast<s32>(lv2_lwq_take(&mutex.q)))
		{
			if (~signal & 1)
			{
				ppu.gpr[3] = CELL_EBUSY;
			}

			return true;
		}

		return false;
	};

	// Free, or just given up: found and taken with no lock over the IDs and
	// no reference counted. That makes no waiter, which is what
	// _sys_lwmutex_destroy asks about, so it is as if it came before a
	// destroy that ran meanwhile.
	bool found = false;

	if (const auto pinned = idm::pin<lv2_obj, lv2_lwmutex>(lwmutex_id))
	{
		found = true;

		if (take_signal(*pinned))
		{
			return not_an_error(ppu.gpr[3]);
		}
	}

	// The thread may sleep on it, so it keeps a reference
	struct
	{
		stx::shared_ptr<lv2_lwmutex> ptr;
		bool ret = false;
		bool slept = false;

		explicit operator bool() const { return !!ptr; }
		lv2_lwmutex* operator->() const { return ptr.get(); }
	}
	mutex{found ? idm::hold<lv2_obj, lv2_lwmutex>(lwmutex_id) : null_ptr};

	if (mutex)
	{
		// The lock over all IDs kept a destroy from coming between the
		// lwmutex being found and the thread being in its queue. idm::enter
		// keeps that for this ID alone: the destroy waits for the thread to
		// be done here before it looks at the queue, or the thread is told
		// that the ID is gone before it has done anything.
		if (const auto entered = idm::enter<lv2_obj, lv2_lwmutex>(lwmutex_id, mutex.ptr.get()))
		{
			bool& slept = mutex.slept;
			mutex.ret = [&, notify = lv2_obj::notify_all_t()](lv2_lwmutex& mutex)
			{
				if (take_signal(mutex))
				{
					return true;
				}

				lv2_obj::prepare_for_sleep(ppu);

				ppu.cancel_sleep = 1;

				if (s32 signal = mutex.try_own(&ppu))
				{
					if (~signal & 1)
					{
						ppu.gpr[3] = CELL_EBUSY;
					}

					ppu.cancel_sleep = 0;
					return true;
				}

				const bool finished = !mutex.sleep(ppu, timeout);
				notify.cleanup();
				slept = true;
				return finished;
			}(*mutex.ptr);
		}
		else
		{
			mutex.ptr = null_ptr;
		}
	}

	if (!mutex)
	{
		if (lwmutex_id >> 24 == lv2_lwmutex::id_base >> 24)
		{
			return { CELL_ESRCH, lwmutex_id };
		}

		return { CELL_ESRCH, "Invalid ID" };
	}

	// Given it by an unlock (it slept): by an unlock2, it says so
	const auto given = [&]
	{
		if (ppu.gpr[3] == CELL_OK && lv2_lwq_given2(&mutex->q, lv2_lwmutex::slot_of(ppu)))
		{
			ppu.gpr[3] = CELL_EBUSY;
		}

		return not_an_error(ppu.gpr[3]);
	};

	if (mutex.ret)
	{
		return mutex.slept ? given() : not_an_error(ppu.gpr[3]);
	}

	while (auto state = +ppu.state)
	{
		if (state & cpu_flag::signal && ppu.state.test_and_reset(cpu_flag::signal))
		{
			break;
		}

		if (is_stopped(state))
		{
			if (lv2_lwq_waits(&mutex->q, lv2_lwmutex::slot_of(ppu)))
			{
				ppu.state += cpu_flag::again;
				return {};
			}

			break;
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

				// Out of the sleepers in one change, or given the lwmutex by an
				// unlock that chose it first: then it is woken, and has it
				if (lv2_lwq_leave(&mutex->q, lv2_lwmutex::slot_of(ppu)))
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

	return given();
}

error_code _sys_lwmutex_trylock(ppu_thread& ppu, u32 lwmutex_id)
{
	ppu.state += cpu_flag::wait;

	sys_lwmutex.trace("_sys_lwmutex_trylock(lwmutex_id=0x%x)", lwmutex_id);

	// Found with no lock; makes no waiter (see _sys_lwmutex_lock)
	struct
	{
		bool found = false;
		bool ret = false;

		explicit operator bool() const { return found; }
	}
	mutex;

	if (const auto pinned = idm::pin<lv2_obj, lv2_lwmutex>(lwmutex_id))
	{
		mutex.found = true;
		mutex.ret = lv2_lwq_trylock(&pinned->q) != 0;
	}

	if (!mutex)
	{
		if (lwmutex_id >> 24 == lv2_lwmutex::id_base >> 24)
		{
			return { CELL_ESRCH, lwmutex_id };
		}

		return { CELL_ESRCH, "Invalid ID" };
	}

	if (!mutex.ret)
	{
		return not_an_error(CELL_EBUSY);
	}

	return CELL_OK;
}

error_code _sys_lwmutex_unlock(ppu_thread& ppu, u32 lwmutex_id)
{
	ppu.state += cpu_flag::wait;

	sys_lwmutex.trace("_sys_lwmutex_unlock(lwmutex_id=0x%x)", lwmutex_id);

	// Nobody waits, as mostly: found and given up with no lock over the IDs
	// and no reference counted
	if (const auto pinned = idm::pin<lv2_obj, lv2_lwmutex>(lwmutex_id))
	{
		if (pinned->try_unlock(false))
		{
			return CELL_OK;
		}
	}
	else
	{
		return CELL_ESRCH;
	}

	// A thread to wake: with a reference
	const auto mutex = idm::hold<lv2_obj, lv2_lwmutex>(lwmutex_id);

	if (mutex)
	{
		[&, notify = lv2_obj::notify_all_t()](lv2_lwmutex& mutex)
	{
		if (mutex.try_unlock(false))
		{
			return;
		}

		// A sleeper to be saved again: the unlock is done again after
		if (mutex.resaving())
		{
			ppu.state += cpu_flag::again;
			return;
		}

		// Given to the best sleeper in one change, no turn
		if (const auto cpu = mutex.reown())
		{
			mutex.awake(cpu);
			notify.cleanup(); // lv2_lwmutex::mutex is not really active 99% of the time, can be ignored
		}
	}(*mutex);
	}

	if (!mutex)
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}

error_code _sys_lwmutex_unlock2(ppu_thread& ppu, u32 lwmutex_id)
{
	ppu.state += cpu_flag::wait;

	sys_lwmutex.warning("_sys_lwmutex_unlock2(lwmutex_id=0x%x)", lwmutex_id);

	if (const auto pinned = idm::pin<lv2_obj, lv2_lwmutex>(lwmutex_id))
	{
		if (pinned->try_unlock(true))
		{
			return CELL_OK;
		}
	}
	else
	{
		return CELL_ESRCH;
	}

	const auto mutex = idm::hold<lv2_obj, lv2_lwmutex>(lwmutex_id);

	if (mutex)
	{
		[&, notify = lv2_obj::notify_all_t()](lv2_lwmutex& mutex)
	{
		if (mutex.try_unlock(true))
		{
			return;
		}

		// A sleeper to be saved again: the unlock is done again after
		if (mutex.resaving())
		{
			ppu.state += cpu_flag::again;
			return;
		}

		// Given to the best sleeper in one change, no turn
		if (const auto cpu = mutex.reown(true))
		{
			mutex.awake(cpu);
			notify.cleanup(); // lv2_lwmutex::mutex is not really active 99% of the time, can be ignored
		}
	}(*mutex);
	}

	if (!mutex)
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}
