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
	lv2_countq_init(&q, static_cast<u64>(std::max<s32>(ar.pop<s32>(), 0)));
}

static_assert(ppu_thread::id_count <= LV2_COUNTQ_SLOTS, "a PPU thread's slot in a semaphore's word is its ID's index");

u32 lv2_sema::slot_of(const ppu_thread& ppu)
{
	using traits = id_manager::id_traits<named_thread<ppu_thread>>;
	return id_manager::get_index(ppu.id, traits::base, traits::step, traits::count, traits::invl_range);
}

// A waiter's rank for a priority semaphore: its thread's priority
static int sema_rank(void* ctx, unsigned slot)
{
	const auto& sema = *static_cast<lv2_sema*>(ctx);
	const auto* ppu = static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(const_cast<retro_atomic_ptr_t*>(&sema.waiter[slot])));
	return ppu ? ppu->prio.load().prio : 0;
}

std::function<void(void*)> lv2_sema::load(utils::serial& ar)
{
	return load_func(make_shared<lv2_sema>(stx::exact_t<utils::serial&>(ar)));
}

void lv2_sema::save(utils::serial& ar)
{
	USING_SERIALIZATION_VERSION(lv2_sync);
	ar(protocol, key, name, max, static_cast<s32>(lv2_countq_value(&q)));
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
		if (lv2_countq_any_waiter(&sema.q))
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
	// counted. Taking a count makes no waiter, which is all
	// sys_semaphore_destroy asks about, so this needs no second look: it
	// is as if it came before a destroy that ran meanwhile.
	if (const auto pinned = idm::pin<lv2_obj, lv2_sema>(sem_id))
	{
		if (lv2_countq_trywait(&pinned->q))
		{
			return CELL_OK;
		}
	}
	else
	{
		return CELL_ESRCH;
	}

	// The thread may sleep on it, so it keeps a reference
	const auto sem = idm::hold<lv2_obj, lv2_sema>(sem_id);

	if (!sem)
	{
		return CELL_ESRCH;
	}

	const u32 slot = lv2_sema::slot_of(ppu);

	// 1: has a count, 0: sleeps, -1: no such semaphore
	const s32 ret = [&, notify = lv2_obj::notify_all_t()]() -> s32
	{
		lv2_obj::prepare_for_sleep(ppu);

		// A waiter once in the word, and may be given a count before it has
		// gone to sleep: that sleep is then cancelled (lv2_obj::sleep gives
		// false), as sys_mutex has it
		ppu.cancel_sleep = 1;
		retro_atomic_store_release_ptr(&sem->waiter[slot], &ppu);

		// A count, or a waiter: one change
		const int got = lv2_countq_wait(&sem->q, slot);
		ensure(got >= 0);

		if (got)
		{
			ppu.cancel_sleep = 0;
			return 1;
		}

		// The lock over all IDs kept a destroy from coming between the
		// semaphore being found and the thread waiting on it. This keeps it:
		// sys_semaphore_destroy marks the ID before it looks for waiters, and
		// the thread asks whether the ID still names the semaphore after it
		// has made itself one. Either the destroy saw the waiter and failed,
		// or the thread is no waiter after all.
		if (!idm::still_named<lv2_obj, lv2_sema>(sem_id, sem.get())) [[unlikely]]
		{
			ppu.cancel_sleep = 0;
			return lv2_countq_leave(&sem->q, slot) ? -1 : 1;
		}

		if (!sem->sleep(ppu, timeout))
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
	}();

	if (ret < 0)
	{
		return CELL_ESRCH;
	}

	if (ret)
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
			if (lv2_countq_waits(&sem->q, slot))
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

				// Out of the waiters in one change, or given a count by a post
				// that chose it first: then it is woken, and has it
				if (lv2_countq_leave(&sem->q, slot))
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
	if (!lv2_countq_trywait(&sem->q))
	{
		return not_an_error(CELL_EBUSY);
	}

	return CELL_OK;
}

error_code sys_semaphore_post(ppu_thread& ppu, u32 sem_id, s32 count)
{
	ppu.state += cpu_flag::wait;

	sys_semaphore.trace("sys_semaphore_post(sem_id=0x%x, count=%d)", sem_id, count);

	if (count <= 0)
	{
		if (!idm::pin<lv2_obj, lv2_sema>(sem_id))
		{
			return CELL_ESRCH;
		}

		return CELL_EINVAL;
	}

	// Found with no lock and no reference counted (see sys_semaphore_wait):
	// the threads it wakes need nothing of it after
	const auto sem = idm::pin<lv2_obj, lv2_sema>(sem_id);

	if (!sem)
	{
		return CELL_ESRCH;
	}

	// A waiter that is to be saved again (a save state being made) is not
	// given a count now: the post is done again after
	for (u32 slot = 0; slot < LV2_COUNTQ_SLOTS; slot++)
	{
		if (lv2_countq_waits(&sem->q, slot))
		{
			const auto* cpu = static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(&sem->waiter[slot]));

			if (cpu && cpu->state & cpu_flag::again)
			{
				ppu.state += cpu_flag::again;
				return {};
			}
		}
	}

	lv2_obj::notify_all_t notify;

	// The count given to the best waiters and the rest counted, or nothing
	// for over the most: one change
	retro_u128_t woken{};

	if (lv2_countq_post(&sem->q, static_cast<u64>(count), static_cast<u64>(sem->max),
		sem->protocol == SYS_SYNC_PRIORITY ? sema_rank : nullptr, &*sem, &woken) != LV2_COUNTQ_POSTED)
	{
		return not_an_error(CELL_EBUSY);
	}

	// Those given a count are woken; waking a thread is for any thread to do
	bool any = false;

	for (u32 slot = 0; slot < LV2_COUNTQ_SLOTS; slot++)
	{
		const bool chosen = slot < 64 ? ((woken.lo >> slot) & 1) : ((woken.hi >> (slot - 64)) & 1);

		if (chosen)
		{
			sem->append(ensure(static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(&sem->waiter[slot]))));
			any = true;
		}
	}

	if (any)
	{
		lv2_obj::awake_all();
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
		value = static_cast<s32>(lv2_countq_value(&sema->q));
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
