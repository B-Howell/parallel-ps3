#pragma once

#include "util/types.hpp"

#include <retro_atomic.h>

u64 convert_to_timebased_time(u64 time);
u64 get_timebased_time();

// Returns some relative time in microseconds, don't change this fact
u64 get_system_time();

// As get_system_time but obeys Clocks scaling setting. Microseconds.
u64 get_guest_system_time(u64 time = umax);

// retro_run's thread: one more frame of the guest's time (sys_time.cpp)
void libretro_guest_clock_grant(double frame_seconds);

// retro_run's thread, once a frame after the grant: the game's watchdog
// looks at the guest's clock (sys_game.cpp)
void lv2_watchdog_tick();

// The watchdog's look at the guest's clock, on its two words (sys_game.cpp):
// @paused_for is how much of the guest's time passed while the emulator was
// paused (not counted). True when it has run out: then it is given up, and
// fires no more.
bool lv2_watchdog_step(retro_atomic_64_t* control, retro_atomic_64_t* start, u64 paused_for);

// The guest's microseconds, unscaled: how much of the guest's time has
// passed, which stops when retro_run does
u64 get_slaved_system_time();

// A time of the slaved clock as get_guest_system_time gives it (sys_time.cpp)
u64 get_guest_system_time_of_slaved(u64 slaved);

// How long of the host's time to wait for @guest_us of the guest's to pass
u64 libretro_guest_wait_as_host(u64 guest_us);

// A 32-bit word that changes, and is notified through retro_waitaddr, each
// time retro_run grants the guest's clock a frame: what a thread pacing
// itself by the guest's time waits on beside its own words
const void* libretro_guest_clock_word();

// The microseconds emulated hardware paces itself by (the RSX audio path's
// periods, the PS Move's updates): in the core the guest's clock, which
// follows retro_run; standalone the host's
inline u64 get_hw_clock_time()
{
	return get_slaved_system_time();
}
