#include "stdafx.h"
#include "sys_timer.h"

#include "Emu/IdManager.h"
#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/timers.hpp"

#include "util/asm.hpp"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "sys_event.h"
#include "sys_process.h"

#include <thread>
#include <new>
#include <queues/retro_deque.h>
#include <retro_atomic.h>
#include <rthreads/retro_waitaddr.h>

LOG_CHANNEL(sys_timer);

// A timer the timer thread holds on to: a shared_ptr<lv2_timer> kept as
// its bytes, so that it sits in a retro_deque and is moved as plain data.
// Made by timer_hold and ended by timer_drop, once each.
struct lv2_timer_held
{
	alignas(shared_ptr<lv2_timer>) uchar bytes[sizeof(shared_ptr<lv2_timer>)];
};

RETRO_DEQUE_DEFINE(lv2_timers, lv2_timer_held);

static const shared_ptr<lv2_timer>& timer_of(const lv2_timer_held* held)
{
	return *std::launder(reinterpret_cast<const shared_ptr<lv2_timer>*>(held->bytes));
}

static lv2_timer_held timer_hold(shared_ptr<lv2_timer>&& timer)
{
	lv2_timer_held held;
	new (held.bytes) shared_ptr<lv2_timer>(std::move(timer));
	return held;
}

static void timer_drop(lv2_timer_held* held)
{
	std::launder(reinterpret_cast<shared_ptr<lv2_timer>*>(held->bytes))->~shared_ptr<lv2_timer>();
}

// The timer thread. Its list of timers is its own: the syscalls hand a
// timer over to it on a lock-free stack (hand_over), and it takes them up
// (take_up) before each look at the timers, so nothing is locked. It waits
// for the next timer with retro_waitaddr on its own word (a timer handed
// over or started, or the thread stopping) and on the guest clock's
// (retro_run granted another frame), for as long of the host's time as the
// guest's should take to the next expiry - no timed sleep of the host's,
// and no poll while the emulator is paused: then it waits for the frames.
struct lv2_timer_thread
{
	lv2_timers_t timers{}; // room for every timer there can be: never grows
	mpsc_stack_t handed{};
	retro_atomic_int_t wake_word{};

	lv2_timer_thread();
	~lv2_timer_thread();

	// Where @timer is in the list, or its count
	usz find(const lv2_timer* timer) const
	{
		const usz count = lv2_timers_count(&timers);

		for (usz i = 0; i < count; i++)
		{
			if (timer_of(lv2_timers_at(&timers, i)).get() == timer)
			{
				return i;
			}
		}

		return count;
	}

	void add(shared_ptr<lv2_timer>&& timer)
	{
		const lv2_timer_held held = timer_hold(std::move(timer));
		ensure(lv2_timers_push_back(&timers, &held));
	}

	void wake()
	{
		retro_atomic_fetch_add_int(&wake_word, 1);
		retro_waitaddr_notify_all(&wake_word);
	}

	// Any thread: @timer into the list (it holds @timer->for_thread till then)
	void hand_over_add(shared_ptr<lv2_timer> timer)
	{
		lv2_timer* const ptr = timer.get();
		ptr->for_thread = std::move(timer);
		mpsc_stack_push(&handed, &ptr->handed_add);
		wake();
	}

	// Any thread: @timer out of the list again
	void hand_over_drop(lv2_timer* timer)
	{
		mpsc_stack_push(&handed, &timer->handed_drop);
		wake();
	}

	// The thread's: what was handed over, in the order it was
	void take_up()
	{
		for (mpsc_stack_node_t* node = mpsc_stack_reverse(mpsc_stack_drain(&handed)); node;)
		{
			// The node is the timer's: read all of it before the timer may go
			const auto handover = static_cast<lv2_timer::thread_handover*>(node);
			lv2_timer* const timer = handover->timer;
			const bool drop = handover->drop;
			node = node->next;

			if (!drop)
			{
				// Destroyed already (sys_timer_destroy between the ID's making and
				// this hand-over, its drop before it): not into the list
				if (retro_atomic_load_acquire_int(&timer->exists))
				{
					add(std::move(timer->for_thread));
				}
				else
				{
					timer->for_thread.reset();
				}
			}
			else if (const usz i = find(timer); i != lv2_timers_count(&timers))
			{
				timer_drop(lv2_timers_at(&timers, i));
				lv2_timers_erase(&timers, i);
			}
		}
	}

	lv2_timer_thread& operator=(thread_state)
	{
		wake();
		return *this;
	}

	void operator()();

	//SAVESTATE_INIT_POS(46); // FREE SAVESTATE_INIT_POS number

	static constexpr auto thread_name = "Timer Thread"sv;
};

lv2_timer::lv2_timer(utils::serial& ar)
	: lv2_obj(1)
	, state(static_cast<int>(ar.pop<u32>()))
	, port(lv2_event_queue::load_ptr(ar, port, "timer"))
	, source(ar)
	, data1(ar)
	, data2(ar)
	, expire(static_cast<s64>(ar.pop<u64>()))
	, period(static_cast<s64>(ar.pop<u64>()))
{
}

void lv2_timer::save(utils::serial& ar)
{
	USING_SERIALIZATION_VERSION(lv2_sync);
	ar(static_cast<u32>(retro_atomic_load_acquire_int(&state))), lv2_event_queue::save_ptr(ar, port.get()), ar(source, data1, data2, static_cast<u64>(retro_atomic_load_acquire_64(&expire)), static_cast<u64>(retro_atomic_load_acquire_64(&period)));
}

u64 lv2_timer::check(u64 _now) noexcept
{
	while (true)
	{
		const u32 _state = static_cast<u32>(retro_atomic_load_acquire_int(&state));

		if (_state == SYS_TIMER_STATE_RUN)
		{
			u64 next = static_cast<u64>(retro_atomic_load_acquire_64(&expire));

			// If aborting, perform the last accurate check for event
			if (_now >= next)
			{
				lv2_obj::notify_all_t notify;

				return turn_call(&turns, [&] { return check_unlocked(_now); });
			}

			return (next - _now);
		}

		break;
	}

	return umax;
}

u64 lv2_timer::check_unlocked(u64 _now) noexcept
{
	const u64 next = static_cast<u64>(retro_atomic_load_acquire_64(&expire));

	if (_now < next || static_cast<u32>(retro_atomic_load_acquire_int(&state)) != SYS_TIMER_STATE_RUN)
	{
		return umax;
	}

	if (port)
	{
		port->send(source, data1, data2, next);
	}

	if (const u64 _period = static_cast<u64>(retro_atomic_load_acquire_64(&period)))
	{
		// Set next expiration time and check again
		const u64 expire0 = utils::add_saturate<u64>(next, _period);
		retro_atomic_store_release_64(&expire, static_cast<s64>(expire0));
		return utils::sub_saturate<u64>(expire0, _now);
	}

	// Stop after oneshot
	retro_atomic_store_release_int(&state, SYS_TIMER_STATE_STOP);
	return umax;
}

lv2_timer_thread::lv2_timer_thread()
{
	ensure(lv2_timers_init(&timers, lv2_timer::id_count));

	Emu.PostponeInitCode([this]()
	{
		idm::select<lv2_obj, lv2_timer>([&](u32 id, lv2_timer&)
		{
			hand_over_add(idm::get_unlocked<lv2_obj, lv2_timer>(id));
		});
	});
}

lv2_timer_thread::~lv2_timer_thread()
{
	// What is still handed over: into the list, and let go of with it
	take_up();

	while (!lv2_timers_empty(&timers))
	{
		timer_drop(lv2_timers_front(&timers));
		lv2_timers_pop_front(&timers);
	}

	lv2_timers_free(&timers);
}

void lv2_timer_thread::operator()()
{
	while (true)
	{
		// Before what was handed over is taken up: a later hand-over wakes
		const int wake = retro_atomic_load_acquire_int(&wake_word);
		take_up();

		if (thread_ctrl::state() == thread_state::aborting)
		{
			break;
		}

		const retro_atomic_int_t* clock_word = static_cast<const retro_atomic_int_t*>(libretro_guest_clock_word());
		const retro_waitaddr_t list[2] =
		{
			{&wake_word, static_cast<u32>(wake)},
			{clock_word, static_cast<u32>(retro_atomic_load_acquire_int(const_cast<retro_atomic_int_t*>(clock_word)))},
		};

		if (Emu.IsPausedOrReady())
		{
			// Until the next frame, or a hand-over
			retro_waitaddr_wait(list, 2, -1);
			continue;
		}

		u64 sleep_time = umax;
		const u64 _now = get_guest_system_time();

		for (usz i = 0, count = lv2_timers_count(&timers); i < count; i++)
		{
			const shared_ptr<lv2_timer>& timer = timer_of(lv2_timers_at(&timers, i));

			while (lv2_obj::check(timer))
			{
				if (thread_ctrl::state() == thread_state::aborting)
				{
					break;
				}

				if (const u64 advised_sleep_time = timer->check(_now))
				{
					if (sleep_time > advised_sleep_time)
					{
						sleep_time = advised_sleep_time;
					}

					break;
				}
			}
		}

		if (sleep_time == umax)
		{
			// No timer running: until one is started or handed over
			retro_waitaddr_wait(list, 1, -1);
		}
		else if (sleep_time)
		{
			// Scale time
			sleep_time = std::min(sleep_time, u64{umax} / 100) * 100 / g_cfg.core.clocks_scale;
			// The guest's time until the next timer: waited for at the pace
			// retro_run comes, and woken early by each frame
			retro_waitaddr_wait(list, 2, static_cast<s64>(libretro_guest_wait_as_host(sleep_time)));
		}
	}
}

error_code sys_timer_create(ppu_thread& ppu, vm::ptr<u32> timer_id)
{
	ppu.state += cpu_flag::wait;

	sys_timer.warning("sys_timer_create(timer_id=*0x%x)", timer_id);

	if (auto ptr = idm::make_ptr<lv2_obj, lv2_timer>())
	{
		// If sys_timer_destroy gets it first, its drop is handed over after
		// this, and the thread takes both up in that order
		g_fxo->get<named_thread<lv2_timer_thread>>().hand_over_add(std::move(ptr));

		ppu.check_state();
		*timer_id = idm::last_id();
		return CELL_OK;
	}

	return CELL_EAGAIN;
}

error_code sys_timer_destroy(ppu_thread& ppu, u32 timer_id)
{
	ppu.state += cpu_flag::wait;

	sys_timer.warning("sys_timer_destroy(timer_id=0x%x)", timer_id);

	auto timer = idm::withdraw<lv2_obj, lv2_timer>(timer_id, [&](lv2_timer& timer) -> CellError
	{
		if (turn_call(&timer.turns, [&] { return lv2_obj::check(timer.port); }))
		{
			return CELL_EISCONN;
		}

		retro_atomic_fetch_sub_int(&timer.exists, 1);
		return {};
	});

	if (!timer)
	{
		return CELL_ESRCH;
	}

	if (timer.ret)
	{
		return timer.ret;
	}

	g_fxo->get<named_thread<lv2_timer_thread>>().hand_over_drop(timer.ptr.get());

	return CELL_OK;
}

error_code sys_timer_get_information(ppu_thread& ppu, u32 timer_id, vm::ptr<sys_timer_information_t> info)
{
	ppu.state += cpu_flag::wait;

	sys_timer.trace("sys_timer_get_information(timer_id=0x%x, info=*0x%x)", timer_id, info);

	sys_timer_information_t _info{};
	const u64 now = get_guest_system_time();

	// Found with no lock over the IDs (idm::check_entered)
	const auto timer = idm::check_entered<lv2_obj, lv2_timer>(timer_id, [&](lv2_timer& timer)
	{
		turn_call(&timer.turns, [&]
		{
			timer.check_unlocked(now);
			timer.get_information(_info);
		});
	});

	if (!timer)
	{
		return CELL_ESRCH;
	}

	ppu.check_state();
	std::memcpy(info.get_ptr(), &_info, info.size());
	return CELL_OK;
}

error_code _sys_timer_start(ppu_thread& ppu, u32 timer_id, u64 base_time, u64 period)
{
	ppu.state += cpu_flag::wait;

	(period ? sys_timer.warning : sys_timer.trace)("_sys_timer_start(timer_id=0x%x, base_time=0x%llx, period=0x%llx)", timer_id, base_time, period);

	const u64 start_time = get_guest_system_time();

	if (period && period < 100)
	{
		// Invalid periodic timer
		return CELL_EINVAL;
	}

	const auto timer = idm::check_entered<lv2_obj, lv2_timer>(timer_id, [&](lv2_timer& timer) -> CellError
	{
		return turn_call(&timer.turns, [&]() -> CellError
		{
			// LV2 Disassembly: Simple nullptr check (assignment test, do not use lv2_obj::check here)
			if (!timer.port)
			{
				return CELL_ENOTCONN;
			}

			timer.check_unlocked(start_time);
			if (static_cast<u32>(retro_atomic_load_acquire_int(&timer.state)) != SYS_TIMER_STATE_STOP)
			{
				return CELL_EBUSY;
			}

			if (!period && start_time >= base_time)
			{
				// Invalid oneshot
				return CELL_ETIMEDOUT;
			}

			const u64 expire = period == 0 ? base_time : // oneshot
				base_time == 0 ? utils::add_saturate(start_time, period) : // periodic timer with no base (using start time as base)
				start_time < utils::add_saturate(base_time, period) ? utils::add_saturate(base_time, period) : // periodic with base time over start time
				[&]() -> u64 // periodic timer base before start time (align to be at least a period over start time)
				{
					// Optimized from a loop in LV2:
					// do
					// {
					// 	  base_time += period;
					// }
					// while (base_time < start_time);

					const u64 start_time_with_base_time_reminder = utils::add_saturate(start_time - start_time % period, base_time % period);

					return utils::add_saturate(start_time_with_base_time_reminder, start_time_with_base_time_reminder < start_time ? period : 0);
				}();

			retro_atomic_store_release_64(&timer.expire, static_cast<s64>(expire));
			retro_atomic_store_release_64(&timer.period, static_cast<s64>(period));
			retro_atomic_store_release_int(&timer.state, SYS_TIMER_STATE_RUN);
			return {};
		});
	});

	if (!timer)
	{
		return CELL_ESRCH;
	}

	if (timer.ret)
	{
		if (timer.ret == CELL_ETIMEDOUT)
		{
			return not_an_error(timer.ret);
		}

		return timer.ret;
	}

	g_fxo->get<named_thread<lv2_timer_thread>>().wake();

	return CELL_OK;
}

error_code sys_timer_stop(ppu_thread& ppu, u32 timer_id)
{
	ppu.state += cpu_flag::wait;

	sys_timer.trace("sys_timer_stop()");

	const auto timer = idm::check_entered<lv2_obj, lv2_timer>(timer_id, [now = get_guest_system_time(), notify = lv2_obj::notify_all_t()](lv2_timer& timer)
	{
		turn_call(&timer.turns, [&]
		{
			timer.check_unlocked(now);
			retro_atomic_store_release_int(&timer.state, SYS_TIMER_STATE_STOP);
		});
	});

	if (!timer)
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}

error_code sys_timer_connect_event_queue(ppu_thread& ppu, u32 timer_id, u32 queue_id, u64 name, u64 data1, u64 data2)
{
	ppu.state += cpu_flag::wait;

	sys_timer.warning("sys_timer_connect_event_queue(timer_id=0x%x, queue_id=0x%x, name=0x%llx, data1=0x%llx, data2=0x%llx)", timer_id, queue_id, name, data1, data2);

	const auto timer = idm::check_entered<lv2_obj, lv2_timer>(timer_id, [&](lv2_timer& timer) -> CellError
	{
		auto found = idm::hold<lv2_obj, lv2_event_queue>(queue_id);

		if (!found)
		{
			return CELL_ESRCH;
		}

		return turn_call(&timer.turns, [&]() -> CellError
		{
			if (lv2_obj::check(timer.port))
			{
				return CELL_EISCONN;
			}

			// Connect event queue
			timer.port   = found;
			timer.source = name ? name : (u64{process_getpid() + 0u} << 32) | u64{timer_id};
			timer.data1  = data1;
			timer.data2  = data2;
			return {};
		});
	});

	if (!timer)
	{
		return CELL_ESRCH;
	}

	if (timer.ret)
	{
		return timer.ret;
	}

	return CELL_OK;
}

error_code sys_timer_disconnect_event_queue(ppu_thread& ppu, u32 timer_id)
{
	ppu.state += cpu_flag::wait;

	sys_timer.warning("sys_timer_disconnect_event_queue(timer_id=0x%x)", timer_id);

	const auto timer = idm::check_entered<lv2_obj, lv2_timer>(timer_id, [now = get_guest_system_time(), notify = lv2_obj::notify_all_t()](lv2_timer& timer) -> CellError
	{
		return turn_call(&timer.turns, [&]() -> CellError
		{
			timer.check_unlocked(now);
			retro_atomic_store_release_int(&timer.state, SYS_TIMER_STATE_STOP);

			if (!lv2_obj::check(timer.port))
			{
				return CELL_ENOTCONN;
			}

			timer.port.reset();
			return {};
		});
	});

	if (!timer)
	{
		return CELL_ESRCH;
	}

	if (timer.ret)
	{
		return timer.ret;
	}

	return CELL_OK;
}

error_code sys_timer_sleep(ppu_thread& ppu, u32 sleep_time)
{
	ppu.state += cpu_flag::wait;

	sys_timer.trace("sys_timer_sleep(sleep_time=%d)", sleep_time);

	return sys_timer_usleep(ppu, sleep_time * u64{1000000});
}

error_code sys_timer_usleep(ppu_thread& ppu, u64 sleep_time)
{
	ppu.state += cpu_flag::wait;

	sys_timer.trace("sys_timer_usleep(sleep_time=0x%llx)", sleep_time);

	if (sleep_time)
	{
		const s64 add_time = g_cfg.core.usleep_addend;

		// Over/underflow checks
		if (add_time >= 0)
		{
			sleep_time = utils::add_saturate<u64>(sleep_time, add_time);
		}
		else
		{
			sleep_time = std::max<u64>(1, utils::sub_saturate<u64>(sleep_time, -add_time));
		}

		lv2_obj::sleep(ppu, g_cfg.core.sleep_timers_accuracy < sleep_timers_accuracy_level::_usleep ? sleep_time : 0);

		if (!lv2_obj::wait_timeout(sleep_time, &ppu, true, true))
		{
			ppu.state += cpu_flag::again;
		}
	}
	else
	{
		std::this_thread::yield();
	}

	return CELL_OK;
}
