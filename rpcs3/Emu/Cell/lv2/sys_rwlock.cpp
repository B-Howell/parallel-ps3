#include "stdafx.h"
#include "sys_rwlock.h"

#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"

#include "util/asm.hpp"

LOG_CHANNEL(sys_rwlock);

lv2_rwlock::lv2_rwlock(utils::serial& ar)
	: protocol(ar)
	, key(ar)
	, name(ar)
{
	// As LV2 keeps it: the writer's ID << 1, or -2 for each reader; bit 0
	// for waiters, who wait again when they are loaded
	const s64 owner = ar.pop<s64>();

	if (owner > 0)
	{
		lv2_rwq_init(&q, static_cast<int>(slot_of_id(static_cast<u32>(owner >> 1))), 0);
		retro_atomic_store_release_int(&writer_id, static_cast<int>(owner >> 1));
	}
	else
	{
		lv2_rwq_init(&q, -1, static_cast<unsigned>(-(owner & -2) >> 1));
	}
}

static_assert(ppu_thread::id_count <= LV2_RWQ_SLOTS, "a PPU thread's slot in an rwlock's word is its ID's index");

u32 lv2_rwlock::slot_of_id(u32 id)
{
	using traits = id_manager::id_traits<named_thread<ppu_thread>>;
	return id_manager::get_index(id, traits::base, traits::step, traits::count, traits::invl_range);
}

std::function<void(void*)> lv2_rwlock::load(utils::serial& ar)
{
	return load_func(make_shared<lv2_rwlock>(stx::exact_t<utils::serial&>(ar)));
}

void lv2_rwlock::save(utils::serial& ar)
{
	USING_SERIALIZATION_VERSION(lv2_sync);

	const s64 waiting = lv2_rwq_any_waiter(&q) ? 1 : 0;
	s64 owner = 0;

	if (lv2_rwq_writer(&q) >= 0)
	{
		owner = s64{retro_atomic_load_acquire_int(&writer_id)} << 1 | waiting;
	}
	else if (const u32 readers = lv2_rwq_readers(&q))
	{
		owner = -2 * s64{readers} | waiting;
	}

	ar(protocol, key, name, owner);
}

// A writer waiting's rank on a priority rwlock: its thread's priority
static int rwlock_rank(void* ctx, unsigned slot)
{
	auto& rw = *static_cast<lv2_rwlock*>(ctx);
	const auto* ppu = static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(&rw.thread[slot]));
	return ppu ? ppu->prio.load().prio : 0;
}

static lv2_rwq_rank_t rwlock_ranking(const lv2_rwlock& rw)
{
	return rw.protocol == SYS_SYNC_PRIORITY ? rwlock_rank : nullptr;
}

// A waiter that is to be saved again (a save state being made) is not
// given the lock now: the unlock is done again after
static bool rwlock_resaving(lv2_rwlock& rw)
{
	if (!lv2_rwq_any_waiter(&rw.q))
	{
		return false;
	}

	for (u32 slot = 0; slot < LV2_RWQ_SLOTS; slot++)
	{
		if (lv2_rwq_waits(&rw.q, slot))
		{
			const auto* cpu = static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(&rw.thread[slot]));

			if (cpu && cpu->state & cpu_flag::again)
			{
				return true;
			}
		}
	}

	return false;
}

// Those given the lock are woken - a writer's ID kept first; waking a
// thread is for any thread to do
static void rwlock_wake(lv2_rwlock& rw, const retro_u128_t& woken)
{
	const int writer = lv2_rwq_writer(&rw.q);
	bool any = false;

	for (u32 slot = 0; slot < LV2_RWQ_SLOTS; slot++)
	{
		const bool chosen = slot < 64 ? ((woken.lo >> slot) & 1) : ((woken.hi >> (slot - 64)) & 1);

		if (chosen)
		{
			auto* const ppu = ensure(static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(&rw.thread[slot])));

			if (static_cast<int>(slot) == writer)
			{
				retro_atomic_store_release_int(&rw.writer_id, static_cast<int>(ppu->id));
			}

			rw.append(ppu);
			any = true;
		}
	}

	if (any)
	{
		lv2_obj::awake_all();
	}
}

error_code sys_rwlock_create(ppu_thread& ppu, vm::ptr<u32> rw_lock_id, vm::ptr<sys_rwlock_attribute_t> attr)
{
	ppu.state += cpu_flag::wait;

	sys_rwlock.warning("sys_rwlock_create(rw_lock_id=*0x%x, attr=*0x%x)", rw_lock_id, attr);

	if (!rw_lock_id || !attr)
	{
		return CELL_EFAULT;
	}

	const auto _attr = *attr;

	const u32 protocol = _attr.protocol;

	if (protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_PRIORITY)
	{
		sys_rwlock.error("sys_rwlock_create(): unknown protocol (0x%x)", protocol);
		return CELL_EINVAL;
	}

	const u64 ipc_key = lv2_obj::get_key(_attr);

	if (auto error = lv2_obj::create<lv2_rwlock>(_attr.pshared, ipc_key, _attr.flags, [&]
	{
		return make_shared<lv2_rwlock>(protocol, ipc_key, _attr.name_u64);
	}))
	{
		return error;
	}

	ppu.check_state();
	*rw_lock_id = idm::last_id<lv2_rwlock>();
	return CELL_OK;
}

error_code sys_rwlock_destroy(ppu_thread& ppu, u32 rw_lock_id)
{
	ppu.state += cpu_flag::wait;

	sys_rwlock.warning("sys_rwlock_destroy(rw_lock_id=0x%x)", rw_lock_id);

	const auto rwlock = idm::withdraw<lv2_obj, lv2_rwlock>(rw_lock_id, [](lv2_rwlock& rw) -> CellError
	{
		if (lv2_rwq_busy(&rw.q))
		{
			return CELL_EBUSY;
		}

		lv2_obj::on_id_destroy(rw, rw.key);
		return {};
	});

	if (!rwlock)
	{
		return CELL_ESRCH;
	}

	if (rwlock.ret)
	{
		return rwlock.ret;
	}

	return CELL_OK;
}

error_code sys_rwlock_rlock(ppu_thread& ppu, u32 rw_lock_id, u64 timeout)
{
	ppu.state += cpu_flag::wait;

	sys_rwlock.trace("sys_rwlock_rlock(rw_lock_id=0x%x, timeout=0x%llx)", rw_lock_id, timeout);

	const u32 slot = lv2_rwlock::slot_of_id(ppu.id);

	// Found with no lock over the IDs. Taking the lock, or waiting for it, is
	// what sys_rwlock_destroy asks about, so a destroy waits for this to be
	// done before it looks (idm::get_entered).
	const auto rwlock = idm::get_entered<lv2_obj, lv2_rwlock>(rw_lock_id, [&, notify = lv2_obj::notify_all_t()](lv2_rwlock& rwlock)
	{
		if (lv2_rwq_tryrlock(&rwlock.q) == LV2_RWQ_OWNED)
		{
			return true;
		}

		lv2_obj::prepare_for_sleep(ppu);

		// A waiter once queued, and may be given the lock before it has gone
		// to sleep: that sleep is then cancelled (lv2_obj::sleep gives
		// false), as sys_mutex_lock has it
		ppu.cancel_sleep = 1;
		retro_atomic_store_release_ptr(&rwlock.thread[slot], &ppu);

		// Taken, or queued: one change, with no turn
		const int got = lv2_rwq_rlock(&rwlock.q, slot);
		ensure(got != LV2_RWQ_BAD);

		if (got != LV2_RWQ_QUEUED || !rwlock.sleep(ppu, timeout))
		{
			// Taken, or given before it went to sleep
			ppu.cancel_sleep = 0;
			return true;
		}

		if (ppu.cancel_sleep != 1)
		{
			notify.cleanup();
		}

		ppu.cancel_sleep = 0;
		return false;
	});

	if (!rwlock)
	{
		return CELL_ESRCH;
	}

	if (rwlock.ret)
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
			if (lv2_rwq_waits(&rwlock->q, slot))
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

				// Out of the readers waiting in one change, or given the lock
				// by an unlock that took it first: then it is woken, and has it
				if (lv2_rwq_rleave(&rwlock->q, slot))
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

error_code sys_rwlock_tryrlock(ppu_thread& ppu, u32 rw_lock_id)
{
	ppu.state += cpu_flag::wait;

	sys_rwlock.trace("sys_rwlock_tryrlock(rw_lock_id=0x%x)", rw_lock_id);

	// As sys_rwlock_rlock: it takes the lock
	const auto rwlock = idm::get_entered<lv2_obj, lv2_rwlock>(rw_lock_id, [](lv2_rwlock& rwlock)
	{
		return lv2_rwq_tryrlock(&rwlock.q) == LV2_RWQ_OWNED;
	});

	if (!rwlock)
	{
		return CELL_ESRCH;
	}

	if (!rwlock.ret)
	{
		return not_an_error(CELL_EBUSY);
	}

	return CELL_OK;
}

error_code sys_rwlock_runlock(ppu_thread& ppu, u32 rw_lock_id)
{
	ppu.state += cpu_flag::wait;

	sys_rwlock.trace("sys_rwlock_runlock(rw_lock_id=0x%x)", rw_lock_id);

	lv2_obj::notify_all_t notify;

	// Found with no lock over the IDs: giving the lock up takes nothing a
	// destroy asks about. 0 done, 1 not a reader, 2 to be done again
	const auto rwlock = idm::get_held<lv2_obj, lv2_rwlock>(rw_lock_id, [&](lv2_rwlock& rwlock) -> int
	{
		if (rwlock_resaving(rwlock))
		{
			return 2;
		}

		// One reader less, and the lock handed to the best writer waiting
		// by the last: one change
		retro_u128_t woken{};

		if (lv2_rwq_runlock(&rwlock.q, rwlock_ranking(rwlock), &rwlock, &woken) != 0)
		{
			return 1;
		}

		rwlock_wake(rwlock, woken);
		return 0;
	});

	if (!rwlock)
	{
		return CELL_ESRCH;
	}

	if (rwlock.ret == 1)
	{
		return CELL_EPERM;
	}

	if (rwlock.ret == 2)
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	return CELL_OK;
}

error_code sys_rwlock_wlock(ppu_thread& ppu, u32 rw_lock_id, u64 timeout)
{
	ppu.state += cpu_flag::wait;

	sys_rwlock.trace("sys_rwlock_wlock(rw_lock_id=0x%x, timeout=0x%llx)", rw_lock_id, timeout);

	const u32 slot = lv2_rwlock::slot_of_id(ppu.id);

	// As sys_rwlock_rlock. 0 taken (or given), 1 its own, 2 asleep
	const auto rwlock = idm::get_entered<lv2_obj, lv2_rwlock>(rw_lock_id, [&, notify = lv2_obj::notify_all_t()](lv2_rwlock& rwlock) -> int
	{
		retro_atomic_store_release_ptr(&rwlock.thread[slot], &ppu);

		switch (lv2_rwq_trywlock(&rwlock.q, slot))
		{
		case LV2_RWQ_OWNED:
			retro_atomic_store_release_int(&rwlock.writer_id, static_cast<int>(ppu.id));
			return 0;
		case LV2_RWQ_MINE:
			return 1;
		default:
			break;
		}

		lv2_obj::prepare_for_sleep(ppu);

		// As sys_rwlock_rlock: queued in one change, asleep after it
		ppu.cancel_sleep = 1;

		const int got = lv2_rwq_wlock(&rwlock.q, slot);
		ensure(got != LV2_RWQ_BAD);

		if (got != LV2_RWQ_QUEUED)
		{
			ppu.cancel_sleep = 0;

			if (got == LV2_RWQ_MINE)
			{
				return 1;
			}

			retro_atomic_store_release_int(&rwlock.writer_id, static_cast<int>(ppu.id));
			return 0;
		}

		if (!rwlock.sleep(ppu, timeout))
		{
			// Given before it went to sleep: the writer
			ppu.cancel_sleep = 0;
			return 0;
		}

		if (ppu.cancel_sleep != 1)
		{
			notify.cleanup();
		}

		ppu.cancel_sleep = 0;
		return 2;
	});

	if (!rwlock)
	{
		return CELL_ESRCH;
	}

	if (rwlock.ret == 0)
	{
		return CELL_OK;
	}

	if (rwlock.ret == 1)
	{
		return CELL_EDEADLK;
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
			if (lv2_rwq_waits(&rwlock->q, slot))
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

				// Out of the writers waiting in one change - the readers it
				// held back let in after - or given the lock by an unlock that
				// took it first: then it is woken, and has it
				lv2_obj::notify_all_t notify;
				retro_u128_t woken{};

				if (lv2_rwq_wleave(&rwlock->q, slot, rwlock_ranking(*rwlock), &*rwlock, &woken))
				{
					ppu.gpr[3] = CELL_ETIMEDOUT;
					rwlock_wake(*rwlock, woken);
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

error_code sys_rwlock_trywlock(ppu_thread& ppu, u32 rw_lock_id)
{
	ppu.state += cpu_flag::wait;

	sys_rwlock.trace("sys_rwlock_trywlock(rw_lock_id=0x%x)", rw_lock_id);

	const u32 slot = lv2_rwlock::slot_of_id(ppu.id);

	// As sys_rwlock_rlock: it takes the lock
	const auto rwlock = idm::get_entered<lv2_obj, lv2_rwlock>(rw_lock_id, [&](lv2_rwlock& rwlock)
	{
		retro_atomic_store_release_ptr(&rwlock.thread[slot], &ppu);

		const int got = lv2_rwq_trywlock(&rwlock.q, slot);

		if (got == LV2_RWQ_OWNED)
		{
			retro_atomic_store_release_int(&rwlock.writer_id, static_cast<int>(ppu.id));
		}

		return got;
	});

	if (!rwlock)
	{
		return CELL_ESRCH;
	}

	if (rwlock.ret == LV2_RWQ_MINE)
	{
		return CELL_EDEADLK;
	}

	if (rwlock.ret != LV2_RWQ_OWNED)
	{
		return not_an_error(CELL_EBUSY);
	}

	return CELL_OK;
}

error_code sys_rwlock_wunlock(ppu_thread& ppu, u32 rw_lock_id)
{
	ppu.state += cpu_flag::wait;

	sys_rwlock.trace("sys_rwlock_wunlock(rw_lock_id=0x%x)", rw_lock_id);

	const u32 slot = lv2_rwlock::slot_of_id(ppu.id);

	lv2_obj::notify_all_t notify;

	// As sys_rwlock_runlock: 0 done, 1 not the writer, 2 to be done again
	const auto rwlock = idm::get_held<lv2_obj, lv2_rwlock>(rw_lock_id, [&](lv2_rwlock& rwlock) -> int
	{
		if (lv2_rwq_writer(&rwlock.q) != static_cast<int>(slot))
		{
			return 1;
		}

		if (rwlock_resaving(rwlock))
		{
			return 2;
		}

		// Handed to the best writer waiting, else to every reader waiting,
		// else free: no turn
		retro_u128_t woken{};

		if (lv2_rwq_wunlock(&rwlock.q, slot, rwlock_ranking(rwlock), &rwlock, &woken) != 0)
		{
			return 1;
		}

		rwlock_wake(rwlock, woken);
		return 0;
	});

	if (!rwlock)
	{
		return CELL_ESRCH;
	}

	if (rwlock.ret == 1)
	{
		return CELL_EPERM;
	}

	if (rwlock.ret == 2)
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	return CELL_OK;
}
