// The emulator's own waits, in the core: for its state (a stop to finish, a
// boot past loading) and for the game (to take an exit request). Standalone
// RPCS3 sleeps a few milliseconds and looks again; here they wait on an
// event, untimed - a change of the emulator's state, or a retro_run:
//
// - a wait does not look again while nothing happens: no polling;
// - each retro_run lets it look once, so the game's time is counted in the
//   frontend's frames;
// - a change of the emulator's state wakes a wait for it at once, without a
//   retro_run.
//
// Built by `make check` from the core's own objects and archives, the same
// link as the core's with this main.

#include "stdafx.h"
#include "Emu/System.h"
#include "Emu/NP/np_contexts.h"
#include "Emu/Cell/timers.hpp"
#include "Utilities/Thread.h"
#include <retro_atomic.h>
#include <rthreads/retro_waitaddr.h>
#include "Emu/IdManager.h"
#include "Emu/system_progress.hpp"
#include "Emu/Cell/Modules/cellOskDialog.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <memory>
#include <vector>

void qt_events_aware_op(int repeat_duration_ms, std::function<bool()> wrapped_op);

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// Until @looks has reached @n, or a few million yields have gone by
static bool reaches(const std::atomic<int>& looks, int n)
{
	for (int i = 0; i < 5'000'000 && looks < n; i++)
		std::this_thread::yield();
	return looks >= n;
}

int main()
{
	// A wait for the game, 16 ms a look in standalone: 60 looks a second
	{
		std::atomic<int> looks{0};
		std::atomic<bool> done{false};
		std::thread waiter([&]
		{
			qt_events_aware_op(16, [&] { return ++looks >= 10; });
			done = true;
		});

		CHECK(reaches(looks, 1));
		// Nothing happening: it does not look again
		for (int i = 0; i < 200000; i++)
			std::this_thread::yield();
		const int idle = looks;
		CHECK(idle == 1);

		// A look a retro_run
		int runs = 0;
		while (looks < 10 && runs < 100)
		{
			const int before = looks;
			libretro_event(); // what retro_run does first
			runs++;
			CHECK(reaches(looks, before + 1));
		}
		waiter.join();
		CHECK(runs == 9);
		std::printf("test_emu_waits: %d look(s) while nothing happened, done after %d retro_runs\n", idle, runs);
	}

	// A wait for the emulator's state: woken by the change itself
	{
		std::atomic<bool> done{false};
		std::thread waiter([&]
		{
			qt_events_aware_op(5, [] { return Emu.IsRunning(); });
			done = true;
		});
		for (int i = 0; i < 200000; i++)
			std::this_thread::yield();
		CHECK(!done);
		Emu.SetTestMode(); // stopped to running
		for (int i = 0; i < 5'000'000 && !done; i++)
			std::this_thread::yield();
		CHECK(done);
		if (!done)
			libretro_event(); // so a failure ends
		waiter.join();
	}

	// A host thread waiting on a word (libretro_wait_on): woken by the word
	// moved, and by the emulator's state changing - not otherwise
	{
		atomic_t<u32> word = 0;
		std::atomic<int> returns{0};
		std::atomic<bool> stop{false};
		std::thread waiter([&]
		{
			while (!stop)
			{
				libretro_wait_on(word, word.load());
				returns++;
			}
		});

		CHECK(!reaches(returns, 1)); // nothing happened: still waiting
		const int idle = returns;
		word++;
		word.notify_all();
		CHECK(reaches(returns, idle + 1)); // the word moved
		const int moved = returns;
		libretro_event();
		CHECK(reaches(returns, moved + 1)); // a change of state, or a retro_run
		stop = true;
		libretro_event();
		waiter.join();
		std::printf("test_emu_waits: libretro_wait_on returned %d time(s) idle, woken by the word and by an event\n", idle);
	}

	// A wait for some of the game's time (libretro_wait_frames): that many
	// retro_runs, whatever the host's clock does
	{
		std::atomic<bool> done{false};
		std::thread waiter([&]
		{
			CHECK(libretro_wait_frames(30));
			done = true;
		});
		for (int i = 0; i < 200000; i++)
			std::this_thread::yield();
		CHECK(!done); // no frame yet: still waiting
		int runs = 0;
		for (; runs < 29; runs++)
		{
			libretro_event();
			for (int i = 0; i < 20000; i++)
				std::this_thread::yield();
		}
		CHECK(!done); // 29 frames: still waiting
		for (int i = 0; i < 1000 && !done; i++)
		{
			libretro_event();
			runs++;
			for (int k = 0; k < 20000 && !done; k++)
				std::this_thread::yield();
		}
		CHECK(done);
		waiter.join();
		std::printf("test_emu_waits: a wait of 30 frames done after %d retro_runs\n", runs);
		// Never before 30 (it was still waiting after 29, above). More than 30
		// is the test's own race, not the wait's: a frame fired before the
		// waiter is back in its wait is not one it saw, and it takes another
		CHECK(runs >= 30);
	}

	// A named thread waiting for frames (libretro_wait_frame, as the progress
	// dialog's server does) is joined with no retro_run at all, as at a close:
	// the join tells it to end through its own word, and that wakes it. A wait
	// on the frames alone never ended there, and the close hung
	{
		std::atomic<int> looks{0};
		auto* waiter = new named_thread("Frame Waiter"sv, [&]
		{
			while (libretro_wait_frame())
				looks++;
		});
		for (int i = 0; i < 100000 && looks < 1; i++)
		{
			libretro_event();
			std::this_thread::yield();
		}
		CHECK(looks >= 1); // it waits for frames, and a frame moves it
		std::atomic<bool> joined{false};
		std::thread joiner([&]
		{
			*waiter = thread_state::finished;
			joined = true;
		});
		for (int i = 0; i < 5'000'000 && !joined; i++)
			std::this_thread::yield();
		CHECK(joined);
		if (!joined)
		{
			std::fprintf(stderr, "test_emu_waits: the join of a thread waiting for a frame never ended\n");
			std::_Exit(1);
		}
		joiner.join();
		delete waiter;
		std::printf("test_emu_waits: a thread waiting for a frame is joined with no retro_run\n");
	}

	// A close's join of the progress dialog's server (Emulator::Kill's first
	// loop joins every thread the emulator keeps), with no retro_run at all:
	// by then the frontend's thread is in the close itself. It is made as a
	// boot makes it and joined as Kill joins it. While it waited for frames
	// alone it never ended, and the close hung
	{
		g_fxo->reset();
		g_fxo->init<named_thread<progress_dialog_server>>();
		usz threads = 0;
		for (const auto& [type, data] : *g_fxo)
		{
			if (type.thread_op)
				threads++;
		}
		// Let them all get to their waits
		for (int i = 0; i < 2'000'000; i++)
			std::this_thread::yield();
		std::atomic<bool> joined{false};
		std::thread joiner([&]
		{
			for (const auto& [type, data] : *g_fxo)
			{
				if (type.thread_op)
					type.thread_op(data, thread_state::finished);
			}
			joined = true;
		});
		for (int i = 0; i < 20'000'000 && !joined; i++)
			std::this_thread::yield();
		CHECK(joined);
		if (!joined)
		{
			std::fprintf(stderr, "test_emu_waits: a close's join of the progress dialog's server never ended\n");
			std::_Exit(1);
		}
		joiner.join();
		g_fxo->clear();
		CHECK(threads > 0);
		std::printf("test_emu_waits: a close joins the progress dialog's server (%zu thread) with no retro_run\n", threads);
	}

	// A thread's wait list holding a retro_atomic word (atomic_wait::list's
	// set_raw): woken by retro_waitaddr_notify_one on that word, and by the
	// thread being told to end
	{
		static retro_atomic_int_t word{0};
		std::atomic<int> woken{0};
		auto* waiter = new named_thread("Raw Word Waiter"sv, [&]
		{
			while (thread_ctrl::state() != thread_state::aborting)
			{
				const u32 seen = static_cast<u32>(retro_atomic_load_acquire_int(&word));
				if (seen >= 3)
				{
					woken = static_cast<int>(seen);
					break;
				}
				thread_ctrl::wait_on_custom<1>([&](atomic_wait::list<3>& list)
				{
					list.template set_raw<0>(&word, seen);
				});
			}
		});
		for (int i = 0; i < 200000; i++)
			std::this_thread::yield();
		CHECK(woken == 0); // nothing changed: still waiting
		for (int k = 0; k < 3; k++)
		{
			retro_atomic_fetch_add_int(&word, 1);
			retro_waitaddr_notify_one(&word);
		}
		for (int i = 0; i < 5'000'000 && woken == 0; i++)
			std::this_thread::yield();
		CHECK(woken == 3);
		*waiter = thread_state::finished;
		delete waiter;

		// And a join wakes one that waits on a word nobody changes
		std::atomic<bool> joined{false};
		auto* idle = new named_thread("Raw Word Idle"sv, [&]
		{
			while (thread_ctrl::state() != thread_state::aborting)
			{
				const u32 seen = static_cast<u32>(retro_atomic_load_acquire_int(&word));
				thread_ctrl::wait_on_custom<1>([&](atomic_wait::list<3>& list)
				{
					list.template set_raw<0>(&word, seen);
				});
			}
		});
		for (int i = 0; i < 200000; i++)
			std::this_thread::yield();
		std::thread joiner([&] { *idle = thread_state::finished; joined = true; });
		for (int i = 0; i < 5'000'000 && !joined; i++)
			std::this_thread::yield();
		CHECK(joined);
		if (!joined)
		{
			std::fprintf(stderr, "test_emu_waits: a join of a thread waiting on a raw word never ended\n");
			std::_Exit(1);
		}
		joiner.join();
		delete idle;
		std::printf("test_emu_waits: a wait list holding a retro_atomic word is woken by its notify and by a join\n");
	}

	// An NP transaction (np_contexts.cpp): no lock and no thread - finished
	// once, by the first of its reply, an abort or its timeout to claim it
	{
		const SceNpCommunicationId com_id{};
		const SceNpCommunicationPassphrase pass{};

		// Two threads claiming each of 20000 transactions at once: one wins
		// each
		int both = 0, neither = 0;
		{
			constexpr int rounds = 20000;
			std::vector<std::unique_ptr<generic_async_transaction_context>> ts;
			for (int i = 0; i < rounds; i++)
			{
				ts.emplace_back(std::make_unique<generic_async_transaction_context>(com_id, pass, 10'000'000));
				ts.back()->start(1);
			}

			std::vector<std::atomic<int>> wins(rounds), arrived(rounds);
			const auto claimer = [&]
			{
				for (int i = 0; i < rounds; i++)
				{
					// Both at the same transaction, then both claim it
					arrived[i]++;
					while (arrived[i] < 2) {}
					if (ts[i]->claim())
						wins[i]++;
				}
			};
			std::thread a(claimer), b(claimer);
			a.join();
			b.join();
			for (int i = 0; i < rounds; i++)
			{
				both += wins[i] == 2;
				neither += wins[i] == 0;
			}
		}
		CHECK(both == 0 && neither == 0);

		// A reply finishes it: the waiter woken with its result, and an abort
		// after is told no
		{
			generic_async_transaction_context t(com_id, pass, 10'000'000);
			t.start(1);
			std::atomic<s32> got{-1};
			named_thread waiter("NP wait test", [&] { got = t.wait_for_completion(); });
			for (int i = 0; i < 200000; i++)
				std::this_thread::yield();
			CHECK(got == -1);
			CHECK(t.claim());
			t.finish(not_an_error(7));
			waiter();
			CHECK(got == 7);
			t.abort_transaction();
			CHECK(t.result_value == 7);
		}

		// Nothing comes: timed out at its deadline, by the wait or by the
		// status asked - and a reply after finds it taken
		{
			generic_async_transaction_context t(com_id, pass, 20'000);
			t.start(1);
			std::atomic<s32> got{-1};
			named_thread waiter("NP timeout test", [&] { got = t.wait_for_completion(); });
			waiter();
			CHECK(got == static_cast<s32>(SCE_NP_COMMUNITY_ERROR_TIMEOUT));
			CHECK(!t.claim());

			generic_async_transaction_context u(com_id, pass, 20'000);
			u.start(2);
			CHECK(!u.get_transaction_status());
			const u64 until = get_system_time() + 40'000;
			while (get_system_time() < until)
				std::this_thread::yield();
			CHECK(u.get_transaction_status() == static_cast<s32>(SCE_NP_COMMUNITY_ERROR_TIMEOUT));
			CHECK(!u.claim());
		}

		std::printf("test_emu_waits: NP transactions finished once (20000 races), woken by the reply, timed out at the deadline\n");
	}

	// The progress dialog's text slot, one word: the inner dialog's text is
	// shown, then the outer's again, then none; open and closed from two
	// threads at once it ends with none open
	{
		using pd = progress_dialog_string_t;
		pd::data_t d;
		d.update_id = 0xabcdef;
		d.text_index = 77;
		d.text_count = 3;
		const pd::data_t r = pd::unpack(pd::pack(d));
		CHECK(r.update_id == 0xabcdef && r.text_index == 77 && r.text_count == 3);
		CHECK(pd::unpack(pd::pack(pd::data_t{})).text_index == umax);

		CHECK(!g_progr_text);
		{
			scoped_progress_dialog outer("outer");
			CHECK(std::string(g_progr_text) == "outer");
			{
				scoped_progress_dialog inner("inner");
				CHECK(std::string(g_progr_text) == "inner");
				CHECK(g_progr_text.load().text_count == 2);
			}
			CHECK(std::string(g_progr_text) == "outer");
			CHECK(g_progr_text.load().text_count == 1);
		}
		CHECK(!g_progr_text);
		CHECK(g_progr_text.load().text_index == umax);

		auto churn = [] { for (int i = 0; i < 3000; i++) { scoped_progress_dialog a("a"); scoped_progress_dialog b("b"); } };
		std::thread t1(churn), t2(churn);
		t1.join();
		t2.join();
		CHECK(!g_progr_text);
		CHECK(g_progr_text.load().text_index == umax);
		std::printf("test_emu_waits: progress text: inner then outer then none, 12000 opens from two threads end with none\n");
	}

	// The OSK's settings as the game sets them: the colour and the pointer's
	// x and y come back as set, each value in its own place, and a reset
	// gives the defaults back
	{
		auto info = std::make_unique<osk_info>();
		const OskDialogBase::color c0 = info->get_base_color();
		CHECK(c0.r == 0.2f && c0.g == 0.2f && c0.b == 0.2f && c0.a == 1.0f);
		info->set_base_color(OskDialogBase::color{0.1f, 0.5f, -0.25f, 0.75f});
		const OskDialogBase::color c = info->get_base_color();
		CHECK(c.r == 0.1f && c.g == 0.5f && c.b == -0.25f && c.a == 0.75f);

		retro_atomic_store_release_64(&info->pointer_pos, osk_info::pack_f32s(-12.5f, 640.0f));
		const s64 pos = retro_atomic_load_acquire_64(&info->pointer_pos);
		CHECK(osk_info::f32_hi(pos) == -12.5f && osk_info::f32_lo(pos) == 640.0f);

		retro_atomic_store_release_int(&info->initial_scale, static_cast<int>(std::bit_cast<u32>(2.0f)));
		retro_atomic_store_release_int(&info->use_separate_windows, 1);
		info->reset();
		CHECK(std::bit_cast<f32>(static_cast<u32>(retro_atomic_load_acquire_int(&info->initial_scale))) == 1.0f);
		CHECK(std::bit_cast<f32>(static_cast<u32>(retro_atomic_load_acquire_int(&info->input_field_background_transparency))) == 1.0f);
		CHECK(retro_atomic_load_acquire_int(&info->use_separate_windows) == 0);
		CHECK(retro_atomic_load_acquire_64(&info->pointer_pos) == 0);
		CHECK(info->get_base_color().a == 1.0f);
		std::printf("test_emu_waits: OSK settings: colour and pointer back as set, reset gives the defaults\n");
	}

	std::printf("test_emu_waits: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
