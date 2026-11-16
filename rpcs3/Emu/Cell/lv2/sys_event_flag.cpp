#include "stdafx.h"
#include "sys_event_flag.h"

#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"

#include "util/asm.hpp"

LOG_CHANNEL(sys_event_flag);

lv2_event_flag::lv2_event_flag(utils::serial& ar)
	: protocol(ar)
	, key(ar)
	, type(ar)
	, name(ar)
{
	lv2_flagq_init(&q, ar.pop<u64>());
}

static_assert(ppu_thread::id_count <= LV2_FLAGQ_SLOTS, "a PPU thread's slot among an event flag's waiters is its ID's index");

u32 lv2_event_flag::slot_of(const ppu_thread& ppu)
{
	using traits = id_manager::id_traits<named_thread<ppu_thread>>;
	return id_manager::get_index(ppu.id, traits::base, traits::step, traits::count, traits::invl_range);
}

// A waiter's rank for a priority flag: its thread's priority
static int event_flag_rank(void* ctx, unsigned slot)
{
	auto& flag = *static_cast<lv2_event_flag*>(ctx);
	const auto* ppu = static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(&flag.thread[slot]));
	return ppu ? ppu->prio.load().prio : 0;
}

// A waiter that is to be saved again (a save state being made) is not
// woken now: the set or cancel is done again after
static bool event_flag_resaving(lv2_event_flag& flag)
{
	for (u32 slot = 0; slot < LV2_FLAGQ_SLOTS; slot++)
	{
		if (lv2_waitset_has(&flag.q.ws, slot))
		{
			const auto* cpu = static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(&flag.thread[slot]));

			if (cpu && cpu->state & cpu_flag::again)
			{
				return true;
			}
		}
	}

	return false;
}

// Those given their result are woken; waking a thread is for any thread to do
static void event_flag_wake(lv2_event_flag& flag, const retro_u128_t& woken)
{
	bool any = false;

	for (u32 slot = 0; slot < LV2_FLAGQ_SLOTS; slot++)
	{
		const bool chosen = slot < 64 ? ((woken.lo >> slot) & 1) : ((woken.hi >> (slot - 64)) & 1);

		if (chosen)
		{
			flag.append(ensure(static_cast<ppu_thread*>(retro_atomic_load_acquire_ptr(&flag.thread[slot]))));
			any = true;
		}
	}

	if (any)
	{
		lv2_obj::awake_all();
	}
}


// Always set result
struct sys_event_store_result
{
	vm::ptr<u64> ptr;
	u64 val = 0;

	~sys_event_store_result() noexcept
	{
		if (ptr)
		{
			cpu_thread::get_current()->check_state();
			*ptr = val;
		}
	}
};

std::function<void(void*)> lv2_event_flag::load(utils::serial& ar)
{
	return load_func(make_shared<lv2_event_flag>(stx::exact_t<utils::serial&>(ar)));
}

void lv2_event_flag::save(utils::serial& ar)
{
	ar(protocol, key, type, name, lv2_flagq_pattern(&q));
}

error_code sys_event_flag_create(ppu_thread& ppu, vm::ptr<u32> id, vm::ptr<sys_event_flag_attribute_t> attr, u64 init)
{
	ppu.state += cpu_flag::wait;

	sys_event_flag.warning("sys_event_flag_create(id=*0x%x, attr=*0x%x, init=0x%llx)", id, attr, init);

	if (!id || !attr)
	{
		return CELL_EFAULT;
	}

	const auto _attr = *attr;

	const u32 protocol = _attr.protocol;

	if (protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_PRIORITY)
	{
		sys_event_flag.error("sys_event_flag_create(): unknown protocol (0x%x)", protocol);
		return CELL_EINVAL;
	}

	const u32 type = _attr.type;

	if (type != SYS_SYNC_WAITER_SINGLE && type != SYS_SYNC_WAITER_MULTIPLE)
	{
		sys_event_flag.error("sys_event_flag_create(): unknown type (0x%x)", type);
		return CELL_EINVAL;
	}

	const u64 ipc_key = lv2_obj::get_key(_attr);

	if (const auto error = lv2_obj::create<lv2_event_flag>(_attr.pshared, ipc_key, _attr.flags, [&]
	{
		return make_shared<lv2_event_flag>(
			_attr.protocol,
			ipc_key,
			_attr.type,
			_attr.name_u64,
			init);
	}))
	{
		return error;
	}

	ppu.check_state();
	*id = idm::last_id<lv2_event_flag>();
	return CELL_OK;
}

error_code sys_event_flag_destroy(ppu_thread& ppu, u32 id)
{
	ppu.state += cpu_flag::wait;

	sys_event_flag.warning("sys_event_flag_destroy(id=0x%x)", id);

	const auto flag = idm::withdraw<lv2_obj, lv2_event_flag>(id, [&](lv2_event_flag& flag) -> CellError
	{
		// Read while threads that found the flag with no lock over the IDs
		// may be joining its waiters (see sys_event_flag_wait), and sets
		// may have one claimed
		if (lv2_flagq_busy(&flag.q))
		{
			return CELL_EBUSY;
		}

		lv2_obj::on_id_destroy(flag, flag.key);
		return {};
	});

	if (!flag)
	{
		return CELL_ESRCH;
	}

	if (flag.ret)
	{
		return flag.ret;
	}

	return CELL_OK;
}

error_code sys_event_flag_wait(ppu_thread& ppu, u32 id, u64 bitptn, u32 mode, vm::ptr<u64> result, u64 timeout)
{
	ppu.state += cpu_flag::wait;

	sys_event_flag.trace("sys_event_flag_wait(id=0x%x, bitptn=0x%llx, mode=0x%x, result=*0x%x, timeout=0x%llx)", id, bitptn, mode, result, timeout);

	// Fix function arguments for external access
	ppu.gpr[3] = -1;
	ppu.gpr[4] = bitptn;
	ppu.gpr[5] = mode;
	ppu.gpr[6] = 0;

	sys_event_store_result store{result};

	if (!lv2_event_flag::check_mode(mode))
	{
		sys_event_flag.error("sys_event_flag_wait(): unknown mode (0x%x)", mode);
		return CELL_EINVAL;
	}

	// The bits are there, as they mostly are: found and taken with no lock
	// and no reference counted. That makes no waiter, which is all
	// sys_event_flag_destroy asks about, so it needs no second look: it is
	// as if it came before a destroy that ran meanwhile.
	if (const auto pinned = idm::pin<lv2_obj, lv2_event_flag>(id))
	{
		if (lv2_flagq_try(&pinned->q, bitptn, mode, &ppu.gpr[6]))
		{
			// TODO: is it possible to return EPERM in this case?
			store.val = ppu.gpr[6];
			return CELL_OK;
		}
	}
	else
	{
		return CELL_ESRCH;
	}

	// The thread may sleep on it, so it keeps a reference
	const auto flag = idm::hold<lv2_obj, lv2_event_flag>(id);

	if (!flag)
	{
		return CELL_ESRCH;
	}

	const u32 slot = lv2_event_flag::slot_of(ppu);
	CellError error{};

	// 1: has the bits, 0: sleeps, 2: claimed by a set or a cancel before it
	// slept (its result given, or being given), -1: error
	const s32 ret = [&, notify = lv2_obj::notify_all_t()]() -> s32
	{
		lv2_obj::prepare_for_sleep(ppu);

		// A waiter once it joins, and may be woken before it has gone to
		// sleep: that sleep is then cancelled (lv2_obj::sleep gives false),
		// as sys_semaphore has it
		ppu.cancel_sleep = 1;
		retro_atomic_store_release_ptr(&flag->thread[slot], &ppu);

		u64 pattern = 0;
		const int got = lv2_flagq_wait(&flag->q, slot, bitptn, mode, flag->type == SYS_SYNC_WAITER_SINGLE, &pattern);

		if (got != LV2_FLAGQ_WAITING)
		{
			ppu.cancel_sleep = 0;
			ppu.gpr[6] = pattern;
			error = got == LV2_FLAGQ_PERM ? CELL_EPERM : CELL_EINVAL;
			return got == LV2_FLAGQ_GOT ? 1 : -1;
		}

		// The lock over all IDs kept a destroy from coming between the flag
		// being found and the thread waiting on it. This keeps it:
		// sys_event_flag_destroy marks the ID before it looks for waiters,
		// and the thread asks whether the ID still names the flag after it
		// has joined them. Either the destroy saw the waiter and failed, or
		// the thread leaves again - unless a set or a cancel took it first.
		if (!idm::still_named<lv2_obj, lv2_event_flag>(id, flag.get())) [[unlikely]]
		{
			ppu.cancel_sleep = 0;

			if (lv2_flagq_leave(&flag->q, slot))
			{
				error = CELL_ESRCH;
				return -1;
			}

			return 2;
		}

		if (!flag->sleep(ppu, timeout))
		{
			// Woken before it went to sleep: given its result
			ppu.cancel_sleep = 0;
			return 2;
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
		return error;
	}

	if (ret == 1)
	{
		store.val = ppu.gpr[6];
		return CELL_OK;
	}

	while (ret == 0)
	{
		const auto state = +ppu.state;

		if (!state)
		{
			break;
		}

		if (state & cpu_flag::signal && ppu.state.test_and_reset(cpu_flag::signal))
		{
			break;
		}

		if (is_stopped(state))
		{
			// Still a waiter: waits again when it is loaded. Else it was
			// claimed, and has its result
			if (lv2_waitset_has(&flag->q.ws, slot))
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

				// Out of the waiters with its time-out, or given what a set
				// or a cancel that claimed it first gives it
				lv2_flagq_timeout(&flag->q, slot);
				break;
			}
		}
		else
		{
			ppu.state.wait(state);
		}
	}

	// Woken with no result (it should not be): out of the waiters, as if
	// its time ran out
	if (lv2_flagq_verdict(&flag->q, slot) == LV2_FLAGQ_PENDING)
	{
		lv2_flagq_timeout(&flag->q, slot);
	}

	// The result, given by whoever claimed it; waited for only when a set
	// or a cancel is giving it as the thread looks (it has its bit cleared
	// and is writing it)
	const int verdict = lv2_flagq_await(&flag->q, slot);
	ppu.gpr[6] = lv2_flagq_result(&flag->q, slot);
	ppu.gpr[3] = verdict == LV2_FLAGQ_TAKEN ? CELL_OK : verdict == LV2_FLAGQ_CANCELED ? +CELL_ECANCELED : +CELL_ETIMEDOUT;

	store.val = ppu.gpr[6];
	return not_an_error(ppu.gpr[3]);
}

error_code sys_event_flag_trywait(ppu_thread& ppu, u32 id, u64 bitptn, u32 mode, vm::ptr<u64> result)
{
	ppu.state += cpu_flag::wait;

	sys_event_flag.trace("sys_event_flag_trywait(id=0x%x, bitptn=0x%llx, mode=0x%x, result=*0x%x)", id, bitptn, mode, result);

	sys_event_store_result store{result};

	if (!lv2_event_flag::check_mode(mode))
	{
		sys_event_flag.error("sys_event_flag_trywait(): unknown mode (0x%x)", mode);
		return CELL_EINVAL;
	}

	u64 pattern{};

	// Found with no lock; see sys_event_flag_wait for why that is enough
	const auto flag = idm::pin<lv2_obj, lv2_event_flag>(id);

	if (!flag)
	{
		return CELL_ESRCH;
	}

	if (!lv2_flagq_try(&flag->q, bitptn, mode, &pattern))
	{
		return not_an_error(CELL_EBUSY);
	}

	store.val = pattern;
	return CELL_OK;
}

error_code sys_event_flag_set(cpu_thread& cpu, u32 id, u64 bitptn)
{
	cpu.state += cpu_flag::wait;

	// Warning: may be called from SPU thread.
	sys_event_flag.trace("sys_event_flag_set(id=0x%x, bitptn=0x%llx)", id, bitptn);

	// The bits are set already, as they often are: found with no lock and
	// no reference counted
	if (const auto pinned = idm::pin<lv2_obj, lv2_event_flag>(id))
	{
		if ((lv2_flagq_pattern(&pinned->q) & bitptn) == bitptn)
		{
			return CELL_OK;
		}
	}
	else
	{
		return CELL_ESRCH;
	}

	// Threads may be woken: with a reference, and one that is the ID's own
	// object (idm::hold)
	const auto flag = idm::hold<lv2_obj, lv2_event_flag>(id);

	if (!flag)
	{
		return CELL_ESRCH;
	}

	if ((lv2_flagq_pattern(&flag->q) & bitptn) == bitptn)
	{
		return CELL_OK;
	}

	if (event_flag_resaving(*flag))
	{
		cpu.state += cpu_flag::again;

		// Fake error for abort
		return not_an_error(CELL_EAGAIN);
	}

	lv2_obj::notify_all_t notify;

	// The bits ORed in, then each waiter they satisfy given what it takes,
	// best first: no lock, no turn
	retro_u128_t woken{};
	lv2_flagq_set(&flag->q, bitptn, flag->protocol == SYS_SYNC_PRIORITY ? event_flag_rank : nullptr, flag.get(), &woken);
	event_flag_wake(*flag, woken);

	return CELL_OK;
}

error_code sys_event_flag_clear(ppu_thread& ppu, u32 id, u64 bitptn)
{
	ppu.state += cpu_flag::wait;

	sys_event_flag.trace("sys_event_flag_clear(id=0x%x, bitptn=0x%llx)", id, bitptn);

	if (const auto flag = idm::pin<lv2_obj, lv2_event_flag>(id))
	{
		lv2_flagq_clear(&flag->q, bitptn);
	}
	else
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}

error_code sys_event_flag_cancel(ppu_thread& ppu, u32 id, vm::ptr<u32> num)
{
	ppu.state += cpu_flag::wait;

	sys_event_flag.trace("sys_event_flag_cancel(id=0x%x, num=*0x%x)", id, num);

	if (num) *num = 0;

	const auto flag = idm::hold<lv2_obj, lv2_event_flag>(id);

	if (!flag)
	{
		return CELL_ESRCH;
	}

	if (event_flag_resaving(*flag))
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	u32 value = 0;
	{
		lv2_obj::notify_all_t notify;

		// Every waiter cancelled with the pattern as it is (the protocol
		// does not matter), in one change
		retro_u128_t woken{};
		value = lv2_flagq_cancel(&flag->q, &woken);
		event_flag_wake(*flag, woken);
	}

	static_cast<void>(ppu.test_stopped());

	if (num) *num = value;
	return CELL_OK;
}

error_code sys_event_flag_get(ppu_thread& ppu, u32 id, vm::ptr<u64> flags)
{
	ppu.state += cpu_flag::wait;

	sys_event_flag.trace("sys_event_flag_get(id=0x%x, flags=*0x%x)", id, flags);

	u64 pattern = 0;

	if (const auto flag = idm::pin<lv2_obj, lv2_event_flag>(id))
	{
		pattern = lv2_flagq_pattern(&flag->q);
	}
	else
	{
		if (flags) *flags = 0;
		return CELL_ESRCH;
	}

	if (!flags)
	{
		return CELL_EFAULT;
	}

	ppu.check_state();

	*flags = pattern;
	return CELL_OK;
}
