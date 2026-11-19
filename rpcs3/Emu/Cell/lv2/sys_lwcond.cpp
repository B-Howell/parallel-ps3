#include "stdafx.h"
#include "sys_lwcond.h"

#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
#include "sys_lwmutex.h"

#include "util/asm.hpp"

#include <rthreads/retro_waitaddr.h>

LOG_CHANNEL(sys_lwcond);

lv2_lwcond::lv2_lwcond(utils::serial& ar)
	: name(ar.pop<be_t<u64>>())
	, lwid(ar)
	, protocol(ar)
	, control(ar.pop<decltype(control)>())
{
	lv2_waitset_init(&waiters);
}

static_assert(ppu_thread::id_count <= LV2_WAITSET_SLOTS, "a PPU thread's slot among an lwcond's waiters is its ID's index");

// What a signal that took a waiter did with it
enum : int
{
	lwcond_pending  = 0,
	lwcond_woken    = 1, // woken, gpr[3] what it returns
	lwcond_on_mutex = 2, // asleep on the lwmutex, to be given it
};

// A waiter's rank: its thread's priority, unless the lwcond is FIFO
static int lwcond_rank(void* ctx, unsigned slot)
{
	auto& cond = *static_cast<lv2_lwcond*>(ctx);
	const auto* ppu = static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(&cond.thread[slot]));
	return ppu ? ppu->get_prio().prio : 0;
}

static lv2_waitset_rank_t lwcond_ranking(const lv2_lwcond& cond)
{
	return cond.protocol == SYS_SYNC_FIFO ? nullptr : lwcond_rank;
}

// The verdict given to the waiter a signal took, before it is woken; a
// waiter whose time ran out meanwhile, waiting for it, woken
static void lwcond_give(lv2_lwcond& cond, u32 slot, int verdict)
{
	retro_atomic_store_release_int(&cond.verdict[slot], verdict);
	lv2_waitset_fence();

	if (retro_atomic_load_acquire_int(&cond.timed_out[slot]))
	{
		retro_waitaddr_notify_all(&cond.verdict[slot]);
	}
}

// A waiter out of the waiters but with no verdict yet: a signal took it and
// is giving it one. Its time-out told first, so the signal wakes it here
static int lwcond_await(lv2_lwcond& cond, u32 slot)
{
	retro_atomic_store_release_int(&cond.timed_out[slot], 1);
	lv2_waitset_fence();

	for (;;)
	{
		const int verdict = retro_atomic_load_acquire_int(&cond.verdict[slot]);

		if (verdict != lwcond_pending)
		{
			return verdict;
		}

		const retro_waitaddr_t w{&cond.verdict[slot], static_cast<u32>(lwcond_pending)};
		retro_waitaddr_wait(&w, 1, -1);
	}
}

// A waiter whose time ran out: out of the waiters (true), or taken by a
// signal, whose verdict is waited for (false)
static bool lwcond_await_or_leave(lv2_lwcond& cond, u32 slot)
{
	retro_atomic_store_release_int(&cond.timed_out[slot], 1);
	lv2_waitset_fence();

	if (lv2_waitset_leave(&cond.waiters, slot))
	{
		return true;
	}

	lwcond_await(cond, slot);
	return false;
}

// A waiter that is to be saved again (a save state being made) is not
// signalled now: the signal is done again after
static bool lwcond_resaving(lv2_lwcond& cond)
{
	if (!lv2_waitset_any(&cond.waiters))
	{
		return false;
	}

	for (u32 slot = 0; slot < LV2_WAITSET_SLOTS; slot++)
	{
		if (lv2_waitset_has(&cond.waiters, slot))
		{
			const auto* cpu = static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(&cond.thread[slot]));

			if (cpu && cpu->state & cpu_flag::again)
			{
				return true;
			}
		}
	}

	return false;
}

// The waiter a signal took given what @mode does. Woken (or appended for
// the caller to wake all, @append) unless it is put to sleep on the lwmutex
static void lwcond_hand(lv2_lwcond& cond, lv2_lwmutex* mutex, ppu_thread* result, u32 mode, bool append)
{
	const u32 slot = lv2_lwmutex::slot_of(*result);

	const auto wake = [&](ppu_thread* cpu)
	{
		if (append)
		{
			lv2_obj::append(cpu);
		}
		else
		{
			cond.awake(cpu);
		}
	};

	if (mode == 2)
	{
		result->gpr[3] = CELL_EBUSY;
	}
	else if (mode == 1 || (mode == 3 && lv2_lwq_any_waiter(&mutex->q)))
	{
		// Respect ordering of the sleep queue: put to sleep on the lwmutex,
		// unless it is free - then it has it
		if (!mutex->try_own(result))
		{
			lwcond_give(cond, slot, lwcond_on_mutex);

			// Mode 3: the calling thread owned it forcefully, and gives it up
			// to the best sleeper - the thread just queued, if no other
			if (mode == 3)
			{
				if (const auto next = mutex->reown())
				{
					wake(next);
				}
			}

			return;
		}
	}

	lwcond_give(cond, slot, lwcond_woken);
	wake(result);
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

			if (lv2_waitset_any(&cond.waiters))
			{
				return CELL_EBUSY;
			}

			old_val = cond.lwmutex_waiters.or_fetch(smin);

			if (old_val != smin)
			{
				// Repeat loop: there are lwmutex waiters inside _sys_lwcond_queue_wait.
				// Descheduled meanwhile
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

			if (ppu.state & cpu_flag::suspend)
			{
				// Test if another signal caused the current thread to be suspended, in which case it needs to wait until the thread wakes up (otherwise the signal may cause unexpected results)
				finished = false;
				return 0;
			}

			if (!lv2_waitset_any(&cond.waiters))
			{
				return 0;
			}

			if (lwcond_resaving(cond) || (cpu && cpu->state & cpu_flag::again) || (mode == 3 && mutex->resaving()))
			{
				ppu.state += cpu_flag::again;
				return 0;
			}

			// Taken off the waiters in one change: the thread named, or the
			// best. No turn
			ppu_thread* result = nullptr;

			if (cpu)
			{
				if (!lv2_waitset_leave(&cond.waiters, lv2_lwmutex::slot_of(*cpu)))
				{
					return 0;
				}

				result = cpu;
			}
			else
			{
				const int slot = lv2_waitset_claim(&cond.waiters, lwcond_ranking(cond), &cond);

				if (slot < 0)
				{
					return 0;
				}

				result = ensure(static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(&cond.thread[slot])));
			}

			lwcond_hand(cond, mutex, result, mode, false);
			return 1;
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

			if (ppu.state & cpu_flag::suspend)
			{
				// Test if another signal caused the current thread to be suspended, in which case it needs to wait until the thread wakes up (otherwise the signal may cause unexpected results)
				finished = false;
				return 0;
			}

			if (!lv2_waitset_any(&cond.waiters))
			{
				return 0;
			}

			if (lwcond_resaving(cond))
			{
				ppu.state += cpu_flag::again;
				return 0;
			}

			// Every waiter taken off in one change, then handed over in the
			// order they would be signalled one by one
			const retro_u128_t taken = lv2_waitset_claim_all(&cond.waiters);
			const auto ranking = lwcond_ranking(cond);

			std::array<u32, LV2_WAITSET_SLOTS> order{};
			u32 count = 0;

			for (u32 slot = 0; slot < LV2_WAITSET_SLOTS; slot++)
			{
				if (slot < 64 ? ((taken.lo >> slot) & 1) : ((taken.hi >> (slot - 64)) & 1))
				{
					order[count++] = slot;
				}
			}

			std::stable_sort(order.begin(), order.begin() + count, [&](u32 a, u32 b)
			{
				const int ra = ranking ? ranking(&cond, a) : 0;
				const int rb = ranking ? ranking(&cond, b) : 0;

				if (ra != rb)
				{
					return ra < rb;
				}

				// Tickets compared by their difference: they wrap
				return static_cast<s32>(static_cast<u32>(retro_atomic_load_acquire_int(&cond.waiters.ticket[a])) - static_cast<u32>(retro_atomic_load_acquire_int(&cond.waiters.ticket[b]))) < 0;
			});

			for (u32 i = 0; i < count; i++)
			{
				lwcond_hand(cond, mutex, ensure(static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(&cond.thread[order[i]]))), mode, true);
			}

			if (count)
			{
				lv2_obj::awake_all();
			}

			return static_cast<int>(count);
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

	// Its slot among the lwcond's waiters and the lwmutex's sleepers
	const u32 slot = lv2_lwmutex::slot_of(ppu);

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

		cond.lwmutex_waiters++;

		// The lwmutex's next owner to wake: none to be saved again, or the
		// wait is done again after
		if (!ppu.loaded_from_savestate && mutex->resaving())
		{
			cond.lwmutex_waiters--;
			mutex->lwcond_waiters--;
			ppu.cancel_sleep = 0;
			ppu.state += cpu_flag::again;
			return;
		}

		retro_atomic_store_release_ptr(&cond.thread[slot], &ppu);
		retro_atomic_store_release_int(&cond.verdict[slot], lwcond_pending);
		retro_atomic_store_release_int(&cond.timed_out[slot], 0);

		if (mutex_sleep)
		{
			// Special: loading state from the point of waiting on lwmutex sleep
			// queue. Asleep on it - or free, and its owner
			if (mutex->try_own(&ppu))
			{
				retro_atomic_store_release_int(&cond.verdict[slot], lwcond_woken);
				ppu.cancel_sleep = 0;
				woken_early = true;
				return;
			}

			retro_atomic_store_release_int(&cond.verdict[slot], lwcond_on_mutex);
		}
		else
		{
			// A waiter, in one change; then the lwmutex given up, which a
			// signal needs to wake it with
			ensure(lv2_waitset_join(&cond.waiters, slot));
		}

		// The lwmutex given up: to its best sleeper, woken on this thread with
		// this thread's sleep, or as the signal. No turn
		ppu_thread* const next_owner = ppu.loaded_from_savestate ? nullptr : mutex->reown();

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
			// Still a waiter, or asleep on the lwmutex - or taken by a signal,
			// whose verdict is waited for
			const bool cond_sleep = lv2_waitset_has(&cond->waiters, slot);
			const bool mutex_sleep = !cond_sleep && lwcond_await(*cond, slot) == lwcond_on_mutex && lv2_lwq_waits(&mutex->q, slot);

			if (!cond_sleep && !mutex_sleep)
			{
				break;
			}

			sstate(mutex_sleep);
			ppu.state += cpu_flag::again;
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

				// Out of the waiters in one change; or taken by a signal, whose
				// verdict says where it is - asleep on the lwmutex, out of its
				// sleepers in one change, or given it already - or woken
				if (lwcond_await_or_leave(*cond, slot))
				{
					ppu.gpr[3] = CELL_ETIMEDOUT;
				}
				else if (retro_atomic_load_acquire_int(&cond->verdict[slot]) == lwcond_on_mutex && lv2_lwq_leave(&mutex->q, slot))
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

	// Given the lwmutex as a sleeper on it by an unlock2: it says so
	if (ppu.gpr[3] == CELL_OK && retro_atomic_load_acquire_int(&cond->verdict[slot]) == lwcond_on_mutex && lv2_lwq_given2(&mutex->q, slot))
	{
		ppu.gpr[3] = CELL_EBUSY;
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
