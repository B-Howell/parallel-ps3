// The guest's clock slaved to retro_run (Emu/Cell/lv2/sys_time.cpp): each
// retro_run grants one frame of the guest's time, which runs smoothly at
// the host's rate within it:
//
// - without retro_run the guest's clock stops within a frame of the last
//   one (the frontend paused), and goes on from there, not jumping ahead;
// - retro_run faster than real time pulls it forward: every frame asked for
//   is had (fast-forward);
// - at the frontend's pace it keeps the frames asked for;
// - inside a frame it moves, as the PS3's timebase does;
// - it never goes backwards on any thread, grants and all;
// - a host time taken as the guest's (get_guest_system_time(time), which
//   deadlines, VBlank and audio timestamps go through) is the guest's time
//   then, never later than now - also while the guest's clock is ahead of
//   the host's, as it is whenever retro_run comes a little faster than the
//   frame rate the core declares (a 60 Hz display, 59.94 declared). It came
//   out as zero then: every timed wait of the game's ended at once, and
//   Demon's Souls played none of its movies;
// - so a timed wait of the game's: its deadline 50 ms away has 50 ms left
//   (lv2_obj::deadline_left, which lv2_obj::wait_timeout goes by), and a
//   20 ms wait takes 20 ms of the guest's time, with retro_run at the
//   declared rate, a little faster, and in fast-forward;
// - the clock emulated hardware paces itself by (get_hw_clock_time: the RSX
//   audio path's periods, the PS Move's updates) is the guest's: it stops
//   through a pause, and a time of it as the timebase's
//   (convert_to_timebased_time, the RSX audio ring's timestamps) is the
//   timebase then, at every pace.
//
// Built by `make check` from the core's own objects and archives, the same
// link as the core's with this main.

#include "stdafx.h"
#include "Emu/Cell/timers.hpp"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/Cell/lv2/sys_rsxaudio.h"
#include "Emu/Cell/Modules/cellAudio.h"
#include "Utilities/Thread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>
#include <vector>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static constexpr double kFrame = 1.0 / 60.0;
static constexpr double kTb = 80000000.0; // the timebase's ticks a second

static double tb_seconds(u64 from, u64 to) { return static_cast<double>(to - from) / kTb; }

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int main()
{
	// Before the first retro_run: the host's
	{
		const u64 a = get_timebased_time();
		sleep_ms(20);
		const u64 b = get_timebased_time();
		CHECK(tb_seconds(a, b) >= 0.019);
	}

	// Paused: one frame, then nothing more
	libretro_guest_clock_grant(kFrame);
	const u64 paused_from = get_timebased_time();
	const u64 slaved_from = get_slaved_system_time();
	const u64 hw_from = get_hw_clock_time();
	sleep_ms(120);
	const u64 paused_to = get_timebased_time();
	const double paused = tb_seconds(paused_from, paused_to);
	CHECK(paused <= kFrame + 0.001);
	CHECK(get_slaved_system_time() - slaved_from <= 17000);
	CHECK(get_hw_clock_time() - hw_from <= 17000);

	// On from where it stopped, not ahead by the pause
	libretro_guest_clock_grant(kFrame);
	{
		const u64 now = get_timebased_time();
		CHECK(tb_seconds(paused_to, now) <= kFrame + 0.001);
	}

	// Fast-forward: ten frames asked for at once, ten frames had
	const u64 ff_from = get_timebased_time();
	for (int i = 0; i < 10; i++)
		libretro_guest_clock_grant(kFrame);
	const double ff = tb_seconds(ff_from, get_timebased_time());
	CHECK(ff >= 8.9 * kFrame);
	CHECK(ff <= 11.1 * kFrame);

	// Inside a frame it moves
	{
		libretro_guest_clock_grant(kFrame);
		const u64 a = get_timebased_time();
		sleep_ms(5);
		const u64 b = get_timebased_time();
		CHECK(b > a);
	}

	// Never backwards, on any thread, while retro_run grants at an uneven pace
	std::atomic<bool> stop{false};
	std::atomic<u32> backwards{0};
	std::atomic<u64> reads{0};
	std::vector<std::thread> readers;
	for (int t = 0; t < 4; t++)
	{
		readers.emplace_back([&]
		{
			u64 last = get_timebased_time();
			u64 n = 0;
			while (!stop)
			{
				const u64 now = get_timebased_time();
				if (now < last)
					backwards++;
				last = now;
				n++;
			}
			reads += n;
		});
	}
	const u64 paced_from = get_timebased_time();
	for (int i = 0; i < 60; i++)
	{
		libretro_guest_clock_grant(kFrame);
		sleep_ms(i % 7 == 0 ? 30 : (i % 3 == 0 ? 2 : 16));
	}
	const double paced = tb_seconds(paced_from, get_timebased_time());
	stop = true;
	for (auto& th : readers)
		th.join();
	CHECK(backwards == 0);
	// Sixty frames asked for: no more than those, no fewer than all but one
	CHECK(paced <= 61.0 * kFrame);
	CHECK(paced >= 58.9 * kFrame);

	// retro_run a little faster than the declared rate, as on a 60 Hz display
	// with 59.94 declared - and much faster, as in fast-forward: the guest's
	// clock gets ahead of the host's
	u32 bad_conversions = 0, late_conversions = 0;
	u64 worst = 0;
	for (int i = 0; i < 240; i++)
	{
		libretro_guest_clock_grant(i < 120 ? 1.0 / 59.94 : kFrame);
		if (i < 120)
			std::this_thread::sleep_for(std::chrono::microseconds(16000));
		else if (i % 4 == 0)
			sleep_ms(1);

		const u64 before = get_guest_system_time();
		const u64 converted = get_guest_system_time(get_system_time());
		const u64 after = get_guest_system_time();
		// The guest's time at that moment: between the two readings around it
		if (converted > after)
			late_conversions++;
		const u64 off = converted < before ? before - converted : 0;
		worst = std::max(worst, off);
		if (off > 1000)
			bad_conversions++;
	}
	CHECK(late_conversions == 0);
	CHECK(bad_conversions == 0);
	std::printf("test_guest_clock: 240 host times taken as the guest's while it runs ahead: %u later than now, %u off by over 1 ms (worst %llu us)\n",
		late_conversions, bad_conversions, static_cast<unsigned long long>(worst));

	// The game's timed waits, with retro_run at three paces from a thread of
	// its own
	{
		static const struct { const char* name; int frame_us; int sleep_us; } paces[] = {
			{ "at the declared rate", 16683, 16683 },
			{ "a little faster (60 Hz display)", 16683, 16000 },
			{ "fast-forward", 16683, 4000 },
		};
		for (const auto& pace : paces)
		{
			std::atomic<bool> run{true};
			std::thread frontend([&]
			{
				while (run)
				{
					libretro_guest_clock_grant(pace.frame_us / 1000000.0);
					std::this_thread::sleep_for(std::chrono::microseconds(pace.sleep_us));
				}
			});
			sleep_ms(100); // the guest's clock ahead by now, where it gets ahead

			// The hardware's clock as the timebase's: the timebase then
			u32 wrong_stamps = 0;
			for (int i = 0; i < 200; i++)
			{
				const u64 before = get_timebased_time();
				const u64 stamp = convert_to_timebased_time(get_hw_clock_time());
				const u64 after = get_timebased_time();
				// To the microsecond the stamp is made from (80 ticks)
				if (stamp + 160 < before || stamp > after + 160)
					wrong_stamps++;
				std::this_thread::sleep_for(std::chrono::microseconds(300));
			}
			CHECK(wrong_stamps == 0);

			u32 expired_at_once = 0, wrong_left = 0, wrong_wait = 0;
			u64 waited_sum = 0;
			// On one of the emulator's threads, as a game's thread is
			named_thread game("Game", [&]
			{
			for (int i = 0; i < 20; i++)
			{
				// A deadline 50 ms of the guest's time away
				const u64 left = lv2_obj::deadline_left(get_guest_system_time() + 50000);
				if (!left)
					expired_at_once++;
				else if (left < 49000 || left > 50000)
					wrong_left++;

				// A 20 ms sleep: 20 ms of the guest's time
				const u64 from = get_guest_system_time();
				lv2_obj::wait_timeout(20000, nullptr, true, true);
				const u64 waited = get_guest_system_time() - from;
				waited_sum += waited;
				if (waited < 19500 || waited > 40000)
					wrong_wait++;
			}
			});
			game();
			run = false;
			frontend.join();
			CHECK(expired_at_once == 0);
			CHECK(wrong_left == 0);
			CHECK(wrong_wait == 0);
			std::printf("test_guest_clock: retro_run %s: of 20 deadlines 50 ms away %u expired at once, %u had the wrong time left; "
				"20 ms waits took %.1f ms of the guest's time, %u wrong; %u of 200 hardware timestamps off the timebase\n",
				pace.name, expired_at_once, wrong_left, waited_sum / 20000.0, wrong_wait, wrong_stamps);
		}
	}

	// The RSX audio path's periods (rsxaudio_periodic_tmr): its thread waits
	// for the next on the guest's clock, with no timer of the system's. A
	// period comes as the guest's time passes - one for each period of it,
	// with retro_run at the declared rate - and none while retro_run does
	// not come: before, a timer of the host's fired and the period's work
	// ran whether the guest's clock had got there or not. And a waiter with
	// no period to wait for wakes when it is cancelled (the thread stopped).
	{
		rsxaudio_periodic_tmr timer;
		std::atomic<u32> periods{0};
		std::atomic<bool> stop{false};
		constexpr u32 rate = 48000; // a period of 1024 bytes: 21.3 ms

		std::atomic<bool> run{true};
		std::thread frontend([&]
		{
			while (run)
			{
				libretro_guest_clock_grant(kFrame);
				std::this_thread::sleep_for(std::chrono::microseconds(16667));
			}
		});
		sleep_ms(50);

		// The data thread first, with no timer: it waits until there is one.
		// The rate comes from this thread, as a syscall's does, with no lock
		std::thread audio([&]
		{
			while (!stop)
			{
				timer.wait([&]
				{
					periods++;
					timer.vtimer_incr(0, get_hw_clock_time());
				});
			}
		});
		sleep_ms(20);
		timer.request_vtimer(0, rate);

		const u64 from = get_hw_clock_time();
		const u32 periods_from = periods;
		sleep_ms(640);
		const u64 guest_us = get_hw_clock_time() - from;
		const u32 had = periods - periods_from;
		const double expected = guest_us / (1024.0 * 1000000.0 / rate);
		CHECK(had + 2 >= expected && had <= expected + 2);

		// The frontend paused: no retro_run, so no guest time, so no period
		// past the one under way
		run = false;
		frontend.join();
		sleep_ms(30);
		const u32 paused_from = periods;
		sleep_ms(200);
		const u32 while_paused = periods - paused_from;
		CHECK(while_paused <= 1);

		// No period at all to wait for: the wait ends when cancelled
		timer.request_vtimer(0, 0);
		sleep_ms(20);
		stop = true;
		const auto t0 = std::chrono::steady_clock::now();
		timer.cancel_wait();
		audio.join();
		const auto took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		CHECK(took_ms < 100);

		std::printf("test_guest_clock: RSX audio periods: %u in %.1f ms of the guest's time (%.1f due), %u while paused 200 ms; a cancelled wait ended in %lld ms\n",
			had, guest_us / 1000.0, expected, while_paused, static_cast<long long>(took_ms));
	}

	// A 4x fast-forward: retro_run every 4 ms of the host's. The guest's
	// clock moves smoothly through each frame, at four times the host's
	// pace: never a jump of several audio periods at once, which cellAudio
	// took for a game that had not fed its buffers - and played silence
	{
		for (int f = 0; f < 10; f++)
		{
			libretro_guest_clock_grant(kFrame);
			sleep_ms(4);
		}

		// A jump is the guest's time moving much more than four times the
		// host's between two looks; a look that came late (the machine busy)
		// is measured against the host's time it took, not counted as a jump
		u64 worst_jump = 0, moved = 0;
		u64 last = get_slaved_system_time();
		auto last_host = std::chrono::steady_clock::now();
		const u64 first = last;
		for (int f = 0; f < 60; f++)
		{
			libretro_guest_clock_grant(kFrame);
			const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
			while (std::chrono::steady_clock::now() < until)
			{
				std::this_thread::sleep_for(std::chrono::microseconds(200));
				const u64 now = get_slaved_system_time();
				const auto now_host = std::chrono::steady_clock::now();
				const u64 host_us = std::chrono::duration_cast<std::chrono::microseconds>(now_host - last_host).count();
				const u64 beyond = now - last > 4 * host_us ? now - last - 4 * host_us : 0;
				worst_jump = std::max<u64>(worst_jump, beyond);
				last = now;
				last_host = now_host;
			}
		}
		moved = last - first;
		CHECK(worst_jump < 4'000);                       // a period is 5.3 ms
		CHECK(moved >= 900'000 && moved <= 1'050'000);   // 60 frames: 1 s
		std::printf("test_guest_clock: 4x fast-forward: guest moved %.1f ms in 60 frames, its largest jump beyond four times the host's %.2f ms\n", moved / 1000.0, worst_jump / 1000.0);
	}

	// cellAudio's periods go by the guest's clock: its ring's time moves a
	// frame's worth for a frame of retro_run, and not while it is paused
	{
		libretro_guest_clock_grant(kFrame);
		sleep_ms(17);
		const u64 a = audio_ringbuffer::get_timestamp();
		for (int f = 0; f < 30; f++)
		{
			libretro_guest_clock_grant(kFrame);
			sleep_ms(17);
		}
		const u64 b = audio_ringbuffer::get_timestamp();
		sleep_ms(300); // the frontend paused
		const u64 c = audio_ringbuffer::get_timestamp();
		CHECK(b - a >= 400'000 && b - a <= 620'000); // 30 frames: 500 ms
		CHECK(c - b <= 20'000);                      // within the frame granted
		std::printf("test_guest_clock: cellAudio's clock: %.1f ms for 30 frames, %.1f ms while paused 300 ms\n", (b - a) / 1000.0, (c - b) / 1000.0);
	}

	// The game's watchdog (sys_game_watchdog_start(1), then never cleared):
	// it runs out after a second of the guest's time - about 60 frames - and
	// once; with the frontend paused it does not run out at all
	{
		retro_atomic_64_t control{}, start{};
		const s64 started = static_cast<s64>((u64{1000000} << 32) | 3); // active, count from now, 1 s

		retro_atomic_store_release_64(&control, started);
		int fired_at = -1;
		for (int f = 0; f < 90 && fired_at < 0; f++)
		{
			libretro_guest_clock_grant(kFrame);
			sleep_ms(17);
			if (lv2_watchdog_step(&control, &start, 0))
				fired_at = f;
		}
		CHECK(fired_at >= 55 && fired_at <= 66);
		CHECK(!lv2_watchdog_step(&control, &start, 0));

		retro_atomic_store_release_64(&control, started);
		libretro_guest_clock_grant(kFrame);
		CHECK(!lv2_watchdog_step(&control, &start, 0));
		sleep_ms(1300); // the frontend paused: no retro_run
		const bool fired_paused = lv2_watchdog_step(&control, &start, 0);
		CHECK(!fired_paused);

		std::printf("test_guest_clock: watchdog of 1 s ran out at frame %d; %s after 1.3 s paused\n", fired_at, fired_paused ? "ran out" : "not");
	}

	// Fast-forward goes as fast as the game makes frames, no faster:
	// retro_run as the core runs it (libretro_guest_frame_due, then the
	// grant), coming every 0.2 ms. The guest's time used to run on a frame
	// every retro_run, whatever the game did: a loading screen's timeouts
	// ran out before their loads, and the game's audio fell behind
	// cellAudio's for good. A game that shows nothing gets at most twice
	// real time; one that shows a frame for every frame (or every second
	// one, at 30 fps) gets every frame asked for; at the frontend's own
	// pace, uneven, every frame is owed even with nothing shown.
	{
		const auto run_for = [](int ms, int flip_every, int sleep_us, const int* pattern_ms, int pattern_n) -> std::pair<int, int>
		{
			int runs = 0, grants = 0;
			const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
			while (std::chrono::steady_clock::now() < until)
			{
				runs++;
				if (libretro_guest_frame_due(kFrame))
				{
					libretro_guest_clock_grant(kFrame);
					grants++;
					if (flip_every && grants % flip_every == 0)
						libretro_guest_flipped();
				}
				if (pattern_n)
					sleep_ms(pattern_ms[runs % pattern_n]);
				else
					std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
			}
			return {runs, grants};
		};

		const u64 a = get_slaved_system_time();
		const auto still = run_for(300, 0, 200, nullptr, 0);
		const u64 moved = get_slaved_system_time() - a;
		// Two frames for every frame of real time, the few owed after the
		// game's last frame, and the scheduler's slack
		CHECK(still.second <= 300 * 2 * 60 / 1000 + 4 + 6);
		CHECK(still.second >= 300 * 60 / 1000);
		CHECK(moved <= 700000);

		// The game shows a frame again
		libretro_guest_flipped();
		const auto every = run_for(150, 1, 200, nullptr, 0);
		CHECK(every.second == every.first);
		const auto half = run_for(150, 2, 200, nullptr, 0);
		CHECK(half.second == half.first);

		libretro_guest_flipped();
		const int uneven[] = {16, 10, 23, 16, 12, 20, 16};
		const auto paced = run_for(400, 0, 0, uneven, 7);
		CHECK(paced.second == paced.first);
		std::printf("test_guest_clock: fast-forward, nothing shown: %d of %d runs owed a frame, guest moved %.1f ms in 300 ms; "
			"a frame shown each: %d of %d; at 30 fps: %d of %d; at the frontend's uneven pace: %d of %d\n",
			still.second, still.first, moved / 1000.0, every.second, every.first, half.second, half.first, paced.second, paced.first);
	}

	std::printf("test_guest_clock: paused 120 ms, guest moved %.1f ms; 10 frames at once, %.1f had; "
		"60 at an uneven pace, %.1f had; %llu reads on 4 threads, %u backwards\n",
		paused * 1000.0, ff / kFrame, paced / kFrame, static_cast<unsigned long long>(reads.load()), backwards.load());
	std::printf("test_guest_clock: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
