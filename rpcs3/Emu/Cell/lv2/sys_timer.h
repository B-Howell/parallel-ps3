#pragma once

#include "sys_event.h"
#include "sys_timer_ctrl.h"
#include "Emu/Memory/vm_ptr.h"

#include <queues/mpsc_stack.h>


// Timer State
enum : u32
{
	SYS_TIMER_STATE_STOP = 0,
	SYS_TIMER_STATE_RUN  = 1,
};

struct sys_timer_information_t
{
	be_t<u64> next_expire; // system_time_t
	be_t<u64> period;
	be_t<u32> timer_state;
	be_t<u32> pad;
};

struct lv2_timer : lv2_obj
{
	static const u32 id_base = 0x11000000;

	// Running or stopped, its next expiry and its period: one 128-bit word
	// changed with compare-and-swap, by the syscalls and the timer thread
	// alike, none waiting for another (sys_timer_ctrl.h)
	lv2_timer_ctrl_t ctrl;

	// The queue it sends to, with the event's words: made whole and put in
	// place with one compare-and-swap, so a firing reads one connection or
	// the other, never half of each
	struct connection
	{
		shared_ptr<lv2_event_queue> port;
		u64 source = 0;
		u64 data1 = 0;
		u64 data2 = 0;
	};

	atomic_ptr<connection> conn;

	// Connected to a queue that still exists
	bool connected() const
	{
		const shared_ptr<connection> c = conn.load();
		return c && lv2_obj::check(c->port);
	}

	// The timer thread's list is the thread's alone: sys_timer_create and
	// sys_timer_destroy hand the timer over to it with one of these, on the
	// thread's mpsc_stack - each used once in the timer's life, so nothing
	// is allocated for it
	// The stack's link is its base, so the node goes back to it with a
	// static_cast
	struct thread_handover : mpsc_stack_node_t
	{
		lv2_timer* timer;
		bool drop;
	};

	thread_handover handed_add{{}, this, false};
	thread_handover handed_drop{{}, this, true};
	shared_ptr<lv2_timer> for_thread; // the thread's reference, until it takes it up

	// Fires it if due at _now, sending the event: the time until it is due
	// next, umax when stopped
	u64 check(u64 _now) noexcept;

	static void send_event(void* ctx, u64 at);

	lv2_timer() noexcept
		: lv2_obj(1)
	{
		lv2_timer_ctrl_init(&ctrl, 0, 0, 0);
	}

	void get_information(sys_timer_information_t& info)
	{
		unsigned running = 0;
		u64 expire = 0, period = 0;
		lv2_timer_ctrl_info(&ctrl, &running, &expire, &period);
		info.timer_state = running ? SYS_TIMER_STATE_RUN : SYS_TIMER_STATE_STOP;
		info.next_expire = expire;
		info.period      = period;
	}

	lv2_timer(utils::serial& ar);
	void save(utils::serial& ar);
};

class ppu_thread;

// Syscalls

error_code sys_timer_create(ppu_thread&, vm::ptr<u32> timer_id);
error_code sys_timer_destroy(ppu_thread&, u32 timer_id);
error_code sys_timer_get_information(ppu_thread&, u32 timer_id, vm::ptr<sys_timer_information_t> info);
error_code _sys_timer_start(ppu_thread&, u32 timer_id, u64 basetime, u64 period); // basetime type changed from s64
error_code sys_timer_stop(ppu_thread&, u32 timer_id);
error_code sys_timer_connect_event_queue(ppu_thread&, u32 timer_id, u32 queue_id, u64 name, u64 data1, u64 data2);
error_code sys_timer_disconnect_event_queue(ppu_thread&, u32 timer_id);
error_code sys_timer_sleep(ppu_thread&, u32 sleep_time);
error_code sys_timer_usleep(ppu_thread&, u64 sleep_time);
