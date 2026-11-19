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
#include "Emu/emu_callbacks.h"
#include "Emu/NP/np_contexts.h"
#include "Emu/Cell/timers.hpp"
#include "Utilities/Thread.h"
#include <retro_atomic.h>
#include <rthreads/retro_waitaddr.h>
#include "Emu/IdManager.h"
#include "Emu/system_progress.hpp"
#include "Emu/Cell/Modules/cellOskDialog.h"
#include "Emu/Cell/Modules/cellCamera.h"
#include "Emu/Cell/Modules/cellSysutil.h"
#include "Emu/RSX/Utils/rsx_utils.h"
#include "Emu/RSX/Core/RSXIOMap.hpp"
#include "Emu/RSX/Overlays/Shaders/shader_loading_dialog.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/NP/net_http_thread.h"
#include "Emu/Cell/Modules/cellAudio.h"
#include "Emu/Cell/Modules/cellDmuxPamf.h"
#include "Emu/RSX/VK/VKCommandStream.h"
#include "Emu/RSX/Common/bitfield.hpp"
#include "Emu/Cell/lv2/sys_spu.h"
#include "Emu/savestate_utils.hpp"
#include "Emu/Memory/vm_locking.h"
#include "Emu/Memory/vm_reservation.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif
#include <memory>
#include <vector>

void qt_events_aware_op(int repeat_duration_ms, std::function<bool()> wrapped_op);

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// Until @looks has reached @n, or a few million yields have gone by
LOG_CHANNEL(waits_test_log, "WaitsTest");

static bool reaches(const std::atomic<int>& looks, int n)
{
	for (int i = 0; i < 5'000'000 && looks < n; i++)
		std::this_thread::yield();
	return looks >= n;
}

int main()
{
	// The camera's savestates, off atomic_t: the same bytes its fields made
	// as u8, bool, u32, u64 and s32, and read back. First, while the emulator
	// is stopped, as a savestate is written
	{
		gem_camera_shared shared;
		retro_atomic_store_release_64(&shared.frame_timestamp_us, 123456789);
		retro_atomic_store_release_int(&shared.width, 320);
		retro_atomic_store_release_int(&shared.height, 240);
		retro_atomic_store_release_int(&shared.size, 76800);
		retro_atomic_store_release_int(&shared.format, CELL_CAMERA_YUV422);
		utils::serial saved;
		shared.save(saved);
		utils::serial expected;
		expected(u64{123456789}, s32{320}, s32{240}, s32{76800}, CellCameraFormat{CELL_CAMERA_YUV422});
		CHECK(saved.data == expected.data && saved.data.size() == 24);

		CHECK(is_savestate_version_compatible({{14, 2}}, true));
		utils::serial in;
		in.set_reading_state(std::move(saved.data));
		gem_camera_shared back(in);
		CHECK(retro_atomic_load_acquire_64(&back.frame_timestamp_us) == 123456789);
		CHECK(retro_atomic_load_acquire_int(&back.width) == 320 && retro_atomic_load_acquire_int(&back.height) == 240);
		CHECK(retro_atomic_load_acquire_int(&back.size) == 76800 && retro_atomic_load_acquire_int(&back.format) == CELL_CAMERA_YUV422);

		// Not initialised: one byte
		auto cam = std::make_unique<camera_context>();
		utils::serial none;
		cam->save(none);
		CHECK(none.data == std::vector<u8>{0});

		// Initialised (closed, so a load opens no camera)
		retro_atomic_store_release_int(&cam->init, 1);
		retro_atomic_store_release_int(&cam->read_mode, CELL_CAMERA_READ_DIRECT);
		retro_atomic_store_release_int(&cam->is_streaming, 1);
		retro_atomic_store_release_int(&cam->is_attached, 1);
		retro_atomic_store_release_int(&cam->frame_num, 77);
		retro_atomic_store_release_int(&cam->is_attached_dirty, 1);
		utils::serial state;
		cam->save(state);
		utils::serial want;
		want(u8{1});
		want(cam->notify_data_map, cam->start_timestamp_us, u8{CELL_CAMERA_READ_DIRECT}, true, true, false, cam->info, cam->attr, u32{77});
		want(true);
		CHECK(state.data == want.data);

		utils::serial cam_in;
		cam_in.set_reading_state(std::move(state.data));
		auto cam_back = std::make_unique<camera_context>(cam_in);
		CHECK(retro_atomic_load_acquire_int(&cam_back->init) == 1);
		CHECK(retro_atomic_load_acquire_int(&cam_back->read_mode) == CELL_CAMERA_READ_DIRECT);
		CHECK(retro_atomic_load_acquire_int(&cam_back->is_streaming) == 1 && retro_atomic_load_acquire_int(&cam_back->is_attached) == 1);
		CHECK(retro_atomic_load_acquire_int(&cam_back->is_open) == 0 && retro_atomic_load_acquire_int(&cam_back->frame_num) == 77);
		CHECK(retro_atomic_load_acquire_int(&cam_back->is_attached_dirty) == 1);

		// The video out setup (rsx::avconf): gamma and state written as the
		// f32 and u32 they were, and read back
		{
			auto conf = std::make_unique<rsx::avconf>();
			conf->set_gamma(1.25f);
			retro_atomic_store_release_int(&conf->state, 1);
			utils::serial av;
			conf->save(av);
			utils::serial av_want;
			av_want(conf->stereo_enabled, conf->format, conf->aspect, conf->resolution_id, conf->scanline_pitch, 1.25f, conf->resolution_x, conf->resolution_y, u32{1}, conf->scan_mode);
			CHECK(av.data == av_want.data);

			CHECK(is_savestate_version_compatible({{10, 4}}, true));
			utils::serial av_in;
			av_in.set_reading_state(std::move(av.data));
			auto back_conf = std::make_unique<rsx::avconf>(av_in);
			CHECK(back_conf->get_gamma() == 1.25f && back_conf->configured());
		}

		// The RSX IO map: its first 512 entries written as the u32s they
		// were, and read back with the reverse map filled in
		{
			auto table = std::make_unique<rsx::rsx_iomap_table>();
			CHECK(table->get_ea(0) == umax && table->get_io(4095) == umax);
			table->set_ea(3, 0x30u << 20);
			table->set_io(0x30, 3u << 20);
			table->set_ea(511, 0x40u << 20);
			table->set_ea(600, 0x50u << 20); // past what is saved
			CHECK(table->get_addr((3u << 20) | 0x1234) == ((0x30u << 20) | 0x1234));
			utils::serial io;
			io(*table);
			std::array<u32, 512> io_want;
			for (u32 i = 0; i < 512; i++)
				io_want[i] = umax;
			io_want[3] = 0x30u << 20;
			io_want[511] = 0x40u << 20;
			utils::serial io_bytes;
			io_bytes(io_want);
			CHECK(io.data == io_bytes.data);

			utils::serial io_in;
			io_in.set_reading_state(std::move(io.data));
			auto back = std::make_unique<rsx::rsx_iomap_table>();
			io_in(*back);
			CHECK(back->get_ea(3) == (0x30u << 20) && back->get_io(0x30) == (3u << 20));
			CHECK(back->get_ea(511) == (0x40u << 20) && back->get_io(0x40) == (511u << 20));
			CHECK(back->get_ea(600) == umax && back->get_io(0x50) == umax);
		}

		// A resource's idle count wraps as the u8 it was
		{
			struct counted : rsx::ref_counted {};
			counted c;
			u32 wraps = 0;
			for (u32 i = 0; i < 300; i++)
				wraps += c.unused_check_count() != static_cast<u8>(i);
			CHECK(wraps == 0);
			c.add_ref();
			CHECK(c.has_refs() && c.unused_check_count() == 0);
			c.release();
			CHECK(!c.has_refs() && c.unused_check_count() == 0); // add_ref started it again
		}

		is_savestate_version_compatible({{0, 25}}, true); // no savestate's versions left behind
		std::printf("test_emu_waits: camera savestates: the bytes atomic_t made, read back\n");
	}

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
			CHECK(reaches(got, 7)); // woken by the reply, not by the join below
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

	// The OSK dialog's abort: an unloaded dialog is left unloaded and says
	// so; any other becomes Abort, also against another thread's changes
	{
		struct stub_osk final : OskDialogBase
		{
			void Create(const osk_params&) override {}
			void Clear(bool) override {}
			void Insert(const std::u16string&) override {}
			void SetText(const std::u16string&) override {}
			void Close(s32) override {}
		};
		stub_osk d;
		CHECK(d.get_state() == OskDialogState::Unloaded);
		CHECK(!d.try_abort());
		CHECK(d.get_state() == OskDialogState::Unloaded);
		d.set_state(OskDialogState::Open);
		CHECK(d.try_abort());
		CHECK(d.get_state() == OskDialogState::Abort);

		int aborted = 0, refused = 0;
		for (int i = 0; i < 2000; i++)
		{
			d.set_state(OskDialogState::Open);
			std::thread closer([&] { d.set_state(OskDialogState::Closed); });
			if (d.try_abort()) aborted++; else refused++;
			closer.join();
			const OskDialogState now = d.get_state();
			CHECK(now == OskDialogState::Abort || now == OskDialogState::Closed);
		}
		CHECK(refused == 0);
		std::printf("test_emu_waits: OSK abort: unloaded stays unloaded, %d opens aborted against a close\n", aborted);
	}

	// libretro_wait_on for a retro_atomic word: returns as the word is set
	// and notified, and on a retro_run - not before either
	{
		retro_atomic_int_t word{0};
		std::atomic<int> returns{0};
		std::atomic<bool> stop{false};
		std::thread waiter([&]
		{
			while (!stop)
			{
				libretro_wait_on(word, static_cast<u32>(retro_atomic_load_acquire_int(&word)));
				returns++;
			}
		});
		for (int i = 0; i < 200000; i++)
			std::this_thread::yield();
		CHECK(returns == 0);
		retro_atomic_store_release_int(&word, 1);
		retro_waitaddr_notify_all(&word);
		CHECK(reaches(returns, 1));
		const int before = returns;
		libretro_event();
		CHECK(reaches(returns, before + 1));
		stop = true;
		libretro_event();
		waiter.join();
		std::printf("test_emu_waits: a wait on a retro_atomic word returns as it is set and on a retro_run\n");
	}

	// A blocking call from the main thread waits on its retro_atomic word: it
	// returns once the call has run on the other thread, not before
	{
		extern emu_callbacks g_emu_callbacks;
		const auto saved = g_emu_callbacks.call_from_main_thread;
		std::thread main_thread;
		std::atomic<bool> handed{false};
		g_emu_callbacks.call_from_main_thread = [&](std::function<void()> func, retro_atomic_int_t* wake_up)
		{
			main_thread = std::thread([func = std::move(func), wake_up, &handed]
			{
				while (!handed)
					std::this_thread::yield();
				for (int i = 0; i < 200000; i++)
					std::this_thread::yield();
				func();
				retro_atomic_store_release_int(wake_up, 1);
				retro_waitaddr_notify_one(wake_up);
			});
		};

		int ran = 0;
		std::thread caller([&]
		{
			handed = true;
			Emu.BlockingCallFromMainThread([&] { ran++; }, false);
			CHECK(ran == 1); // returned only after the call ran
		});
		caller.join();
		main_thread.join();
		CHECK(ran == 1);
		g_emu_callbacks.call_from_main_thread = saved;
		std::printf("test_emu_waits: a blocking call from the main thread returns after it ran\n");
	}

	// The shader loading dialog closes once the main thread has run every
	// call it queued: its count is a retro_atomic word, and close() wakes on
	// the last call, not before
	{
		struct stub_dialog final : MsgDialogBase
		{
			std::atomic<int> calls{0};
			void Create(const std::string&, const std::string&) override { calls++; }
			void Close(bool) override {}
			void SetMsg(const std::string&) override {}
			void ProgressBarSetMsg(u32, const std::string&) override { calls++; }
			void ProgressBarReset(u32) override {}
			void ProgressBarInc(u32, u32) override { calls++; }
			void ProgressBarSetValue(u32, u32) override { calls++; }
			void ProgressBarSetLimit(u32, u32) override { calls++; }
		};
		extern emu_callbacks g_emu_callbacks;
		const auto saved = g_emu_callbacks.call_from_main_thread;
		std::vector<std::function<void()>> queued;
		g_emu_callbacks.call_from_main_thread = [&](std::function<void()> func, retro_atomic_int_t*)
		{
			queued.push_back(std::move(func));
		};

		auto stub = std::make_shared<stub_dialog>();
		rsx::shader_loading_dialog d;
		d.dlg = stub;
		d.update_msg(0, "a");
		d.inc_value(0, 1);
		d.set_value(1, 2);
		d.set_limit(1, 3);
		CHECK(retro_atomic_load_acquire_int(&d.ref_cnt) == 4);
		CHECK(queued.size() == 4);

		std::atomic<int> closed{0};
		std::thread closer([&] { d.close(); closed = 1; });
		for (std::size_t i = 0; i < queued.size(); i++)
		{
			for (int j = 0; j < 200000; j++)
				std::this_thread::yield();
			CHECK(closed == 0); // a call still queued
			queued[i]();
		}
		CHECK(reaches(closed, 1));
		closer.join();
		CHECK(stub->calls == 4);
		CHECK(retro_atomic_load_acquire_int(&d.ref_cnt) == 0);
		g_emu_callbacks.call_from_main_thread = saved;
		std::printf("test_emu_waits: the shader loading dialog closes after its last queued call\n");
	}

	// The net_http thread: a caller's work runs on that thread, and the
	// caller wakes on its own completion word as it has run, 200 times from
	// two callers at once
	{
		std::atomic<int> ran{0}, returned{0};
		std::thread::id worker{};
		{
			np::net_http_thread t;
			const auto caller = [&]
			{
				for (int i = 0; i < 100; i++)
				{
					CHECK(t.run([&] { worker = std::this_thread::get_id(); ran++; }));
					returned++;
				}
			};
			std::thread a(caller), b(caller);
			// Each woken by its own work having run; a caller never woken
			// cannot be joined, so the test ends here
			if (!reaches(returned, 200))
			{
				std::fprintf(stderr, "%s:%d: check failed: a net_http caller not woken\n", __FILE__, __LINE__);
				std::_Exit(1);
			}
			a.join();
			b.join();
			CHECK(ran == 200);
			CHECK(worker != std::thread::id{} && worker != std::this_thread::get_id());
		}
		std::printf("test_emu_waits: the net_http thread runs its callers' work and wakes each, 200 calls\n");
	}

	// An audio port, saved bitwise: its state and its volume step in the
	// bytes atomic_t made (a u32, then two floats in order), and the state's
	// compare-and-swap giving the state it was, as compare_and_swap did
	{
		audio_port p;
		CHECK(p.get_state() == audio_port_state::closed);
		p.set_state(audio_port_state::started);
		p.set_level_set({0.5f, -0.25f});

		u8 bytes[sizeof(audio_port)];
		std::memcpy(bytes, &p, sizeof(p));
		const u32 state_bytes = static_cast<u32>(audio_port_state::started);
		const f32 level_bytes[2] = {0.5f, -0.25f};
		CHECK(std::memcmp(bytes + offsetof(audio_port, state), &state_bytes, 4) == 0);
		CHECK(std::memcmp(bytes + offsetof(audio_port, level_set), level_bytes, 8) == 0);

		audio_port q(p); // the copy a savestate load makes
		CHECK(q.get_state() == audio_port_state::started);
		CHECK(q.get_level_set().value == 0.5f && q.get_level_set().inc == -0.25f);

		CHECK(p.cas_state(audio_port_state::started, audio_port_state::opened) == audio_port_state::started);
		CHECK(p.cas_state(audio_port_state::started, audio_port_state::closed) == audio_port_state::opened);
		CHECK(p.get_state() == audio_port_state::opened);

		CHECK(!p.cas_level_set({0.5f, 0.0f}, {1.0f, 0.0f}));
		CHECK(p.cas_level_set({0.5f, -0.25f}, {0.5f, 0.0f}));
		CHECK(p.get_level_set().value == 0.5f && p.get_level_set().inc == 0.0f);
		std::printf("test_emu_waits: an audio port in the bytes atomic_t made, its state swapped as compare_and_swap did\n");
	}

	// The demuxer's queue (in the game's memory, its count a word in a byte
	// array): a thread waiting on an empty queue wakes as an entry goes in,
	// 500 times; it holds 4 at most and gives them back in order
	{
		static u32 entries[4];
		static dmux_pamf_hle_spurs_queue<u32, 4> q;
		q.init(entries);
		CHECK(q.size() == 0);
		for (u32 i = 0; i < 4; i++)
			CHECK(q.emplace(i));
		CHECK(!q.emplace(9u));
		u32 v = 0;
		CHECK(q.peek(v) && v == 0 && q.size() == 4);
		for (u32 i = 0; i < 4; i++)
			CHECK(q.pop(v) && v == i);
		CHECK(!q.pop());

		std::atomic<int> taken{0};
		named_thread consumer("dmux queue test", [&]
		{
			for (int i = 0; i < 500; i++)
			{
				q.wait();
				u32 got = 0;
				if (!q.pop(got) || got != static_cast<u32>(i))
					return;
				taken++;
			}
		});
		bool all = true;
		for (int i = 0; i < 500 && all; i++)
		{
			for (int j = 0; j < 2000; j++)
				std::this_thread::yield();
			CHECK(q.emplace(static_cast<u32>(i)));
			all = reaches(taken, i + 1);
		}
		CHECK(all);
		if (!all)
		{
			std::fprintf(stderr, "%s:%d: check failed: the demuxer queue's waiter not woken\n", __FILE__, __LINE__);
			std::_Exit(1);
		}
		consumer();
		std::printf("test_emu_waits: the demuxer queue wakes its waiter, 500 entries, and keeps order\n");
	}

	// The Vulkan driver manager: a drain returns once the driver thread has
	// taken up the last event completed before it, woken by the thread, not
	// by its own join; 200 rounds, the events raised out of order
	{
		vk::driver_manager_thread drv;
		std::atomic<int> drained{0};
		bool all = true;
		for (int i = 0; i < 200 && all; i++)
		{
			drv.notify_completed(static_cast<u64>(2 * i + 2));
			drv.notify_completed(static_cast<u64>(2 * i + 1)); // lower: kept at 2 * i + 2
			named_thread drainer("VK drain test", [&] { drv.drain(); drained++; });
			all = reaches(drained, i + 1);
			if (!all)
			{
				std::fprintf(stderr, "%s:%d: check failed: a drain not woken by the driver thread\n", __FILE__, __LINE__);
				std::_Exit(1);
			}
			drainer();
		}
		CHECK(all);
		drv = thread_state::aborting;
		drv();
		std::printf("test_emu_waits: the Vulkan driver manager wakes each drain, 200 rounds\n");
	}

	// The RSX's atomic bitmask: set, test, clear and wait on one retro_atomic
	// word, 32 bits and 64 (the top bit of each too); a waiter woken as its
	// mask changes, 300 times
	{
		enum class m32 : u32 { a = 1, b = 2, top = 0x80000000u };
		enum class m64 : u64 { a = 1, high = 1ull << 40, top = 1ull << 63 };

		rsx::atomic_bitmask_t<m32> f;
		CHECK(!f);
		f |= m32::a;
		CHECK((f & m32::a) && !(f & m32::b));
		CHECK(!f.test_and_set(m32::top));
		CHECK(f.test_and_set(m32::top));
		CHECK(static_cast<u32>(f.load()) == 0x80000001u);
		CHECK(f.clear(m32::a) == 0x80000000u); // what is left
		f &= m32::top;
		CHECK(static_cast<u32>(f.load()) == 0x80000000u);
		f.clear();
		CHECK(!f);

		rsx::atomic_bitmask_t<m64, u64> g;
		g |= m64::high;
		g |= m64::top;
		CHECK((g & m64::high) && (g & m64::top) && !(g & m64::a));
		CHECK(g.clear(m64::top) == (1ull << 40));
		CHECK(static_cast<u64>(g.load()) == (1ull << 40));

		std::atomic<int> woken{0};
		f.store(m32::a);
		named_thread waiter("bitmask wait test", [&]
		{
			for (int i = 0; i < 300; i++)
			{
				const m32 want = (i & 1) ? m32::a : m32::b;
				while (f.load() != want)
					f.wait(f.load());
				woken++;
			}
		});
		bool all = true;
		for (int i = 0; i < 300 && all; i++)
		{
			for (int j = 0; j < 2000; j++)
				std::this_thread::yield();
			f.store((i & 1) ? m32::a : m32::b);
			f.notify_all();
			all = reaches(woken, i + 1);
		}
		if (!all)
		{
			std::fprintf(stderr, "%s:%d: check failed: a bitmask waiter not woken\n", __FILE__, __LINE__);
			std::_Exit(1);
		}
		waiter();
		std::printf("test_emu_waits: the RSX bitmask: 32 and 64 bits, its top bits, and 300 wakes\n");
	}

	// A log channel: 32 bytes, its enabled level a retro_atomic word; each
	// message finds its channel and its level from its own address
	{
		const logs::channel& ch = waits_test_log;
		CHECK(reinterpret_cast<uptr>(&ch) % 32 == 0);
		const logs::message* msgs[] = {&ch.fatal, &ch.error, &ch.todo, &ch.success, &ch.warning, &ch.notice, &ch.trace};
		for (int i = 0; i < 7; i++)
		{
			CHECK((*msgs[i]).operator->() == &ch);
			CHECK(static_cast<logs::level>(*msgs[i]) == static_cast<logs::level>(i + 1));
		}
		const logs::level saved_level = ch.get_enabled();
		ch.set_enabled(logs::level::error);
		CHECK(static_cast<bool>(ch.fatal) && static_cast<bool>(ch.error));
		CHECK(!static_cast<bool>(ch.warning) && !static_cast<bool>(ch.trace));
		ch.set_enabled(logs::level::trace);
		CHECK(static_cast<bool>(ch.trace));
		ch.set_enabled(saved_level);
		std::printf("test_emu_waits: a log channel's messages find it and their level, and its level filters them\n");
	}

	// An SPU thread group's priority, one retro_atomic word: made with its
	// priority, read whole, and changed by two threads at once with none of
	// their changes lost (400000 in all), the priority kept by each
	{
		auto group = std::make_unique<lv2_spu_group>("prio test", 1, 100, 0, nullptr, true, 0);
		CHECK(group->get_prio().prio == 100 && group->get_prio().order == 0);
		std::atomic<int> arrived{0};
		const auto bump = [&](int cpu)
		{
#ifdef __linux__
			// Each on a CPU of its own: left to the scheduler, the two did
			// not overlap enough to lose a change with a plain store
			cpu_set_t set;
			CPU_ZERO(&set);
			CPU_SET(cpu % std::max(1u, std::thread::hardware_concurrency()), &set);
			pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
			(void)cpu;
#endif
			arrived++;
			while (arrived < 2)
				std::this_thread::yield();
			for (int i = 0; i < 200000; i++)
				group->prio_op([](spu_thread::spu_prio_t& p) { p.order = p.order + 1; });
		};
		std::thread a(bump, 0), b(bump, 1);
		a.join();
		b.join();
		CHECK(group->get_prio().order == 400000 && group->get_prio().prio == 100);
		group->prio_op([](spu_thread::spu_prio_t& p) { p.prio = 3; });
		CHECK(group->get_prio().prio == 3 && group->get_prio().order == 400000);
		std::printf("test_emu_waits: an SPU group's priority, changed by two threads at once, 400000 changes kept\n");
	}

	// The lock-free array and FIFO (Utilities/lockless.h): two threads
	// growing one array at once see the same element at each index, 2000
	// times; and a FIFO's places, taken by two threads at once, are each
	// taken once, and it empties to 0 as the last is let go
	{
		int mismatched = 0;
		for (int round = 0; round < 2000; round++)
		{
			auto arr = std::make_unique<lf_array<u64, 4>>();
			u64* seen[2][32]{};
			std::atomic<int> arrived{0};
			const auto grow = [&](int t)
			{
				arrived++;
				while (arrived < 2) {}
				for (int i = 0; i < 32; i++)
					seen[t][i] = &(*arr)[i];
			};
			std::thread a(grow, 0), b(grow, 1);
			a.join();
			b.join();
			for (int i = 0; i < 32; i++)
				mismatched += seen[0][i] != seen[1][i];
		}
		CHECK(mismatched == 0);

		lf_fifo<u32, 8> fifo;
		std::vector<std::atomic<int>> taken(2000);
		std::atomic<int> arrived{0};
		const auto take = [&]
		{
			arrived++;
			while (arrived < 2) {}
			for (int i = 0; i < 1000; i++)
				taken[fifo.push_begin()]++;
		};
		std::thread a(take), b(take);
		a.join();
		b.join();
		int once = 0;
		for (auto& t : taken)
			once += t == 1;
		CHECK(once == 2000 && fifo.size() == 2000);
		CHECK(fifo.pop_end(1999) == 1999 && fifo.peek() == 1999);
		CHECK(fifo.pop_end() == 0 && fifo.size() == 0 && fifo.peek() == 0);
		std::printf("test_emu_waits: the lock-free array grown by two threads at once (2000 times) and the FIFO's places taken once each\n");
	}

	// A raw SPU's place (spu_thread::try_inc_below): raised only below its
	// limit, and of two threads raising it to its last place at once, one
	// gets it, 20000 times
	{
		retro_atomic_int_t word{0};
		CHECK(spu_thread::try_inc_below(&word, 2) && spu_thread::try_inc_below(&word, 2));
		CHECK(!spu_thread::try_inc_below(&word, 2) && retro_atomic_load_acquire_int(&word) == 2);

		constexpr int rounds = 20000;
		std::vector<std::atomic<int>> wins(rounds), arrived(rounds), done(rounds);
		const auto taker = [&](bool resets)
		{
			for (int i = 0; i < rounds; i++)
			{
				arrived[i]++;
				while (arrived[i] < 2) {}
				wins[i] += spu_thread::try_inc_below(&word, 5);
				done[i]++;
				if (resets)
				{
					while (done[i] < 2) {}
					retro_atomic_store_release_int(&word, 4);
					done[i] = 3;
				}
				else
				{
					while (done[i] < 3) {}
				}
			}
		};
		retro_atomic_store_release_int(&word, 4);
		std::thread a(taker, true), b(taker, false);
		a.join();
		b.join();
		int not_one = 0;
		for (auto& w : wins)
			not_one += w != 1;
		CHECK(not_one == 0);
		std::printf("test_emu_waits: a raw SPU's place raised below its limit only, 20000 races with one winner each\n");
	}

	// An SPU channel, its word a retro_atomic_64_t the recompilers reach at
	// offset 0: a value pushed and popped, only one of two pushes at once
	// into an empty channel lands (20000 times), and a waiter on its upper
	// half woken by a push, 300 times
	{
		spu_channel ch;
		u32 v = 0;
		CHECK(!ch.try_pop(v) && ch.get_count() == 0);
		CHECK(ch.try_push(7) && !ch.try_push(8) && ch.get_count() == 1);
		CHECK(ch.try_read(v) && v == 7);
		CHECK(ch.try_pop(v) && v == 7 && ch.get_count() == 0);
		ch.set_value(0x1234);
		CHECK(ch.get_value() == 0x1234 && ch.get_count() == 1);
		CHECK(ch.pop() == 0x1234);

		constexpr int rounds = 20000;
		std::vector<std::atomic<int>> wins(rounds), arrived(rounds), done(rounds);
		const auto pusher = [&](bool resets)
		{
			for (int i = 0; i < rounds; i++)
			{
				arrived[i]++;
				while (arrived[i] < 2) {}
				wins[i] += ch.try_push(resets ? 1 : 2);
				done[i]++;
				if (resets)
				{
					while (done[i] < 2) {}
					u32 out = 0;
					ch.try_pop(out);
					done[i] = 3;
				}
				else
				{
					while (done[i] < 3) {}
				}
			}
		};
		{
			std::thread a(pusher, true), b(pusher, false);
			a.join();
			b.join();
		}
		int not_one = 0;
		for (auto& w : wins)
			not_one += w != 1;
		CHECK(not_one == 0);

		std::atomic<int> woken{0}, marked{0};
		std::atomic<bool> waiting{false};
		std::thread waiter([&]
		{
			for (int i = 0; i < 300; i++)
			{
				while (marked < i + 1) {}
				const u64 seen = ch.load(); // the waiter's mark
				waiting = true;
				while (ch.load() == seen)
				{
					const retro_waitaddr_t w{ch.wait_word(), ch.wait_word_of(seen)};
					retro_waitaddr_wait(&w, 1, -1);
				}
				woken++;
			}
		});
		bool all = true;
		for (int i = 0; i < 300 && all; i++)
		{
			// A waiter marked, as pop_wait marks it, then a push wakes it
			retro_atomic_store_release_64(&ch.data, static_cast<s64>(spu_channel::bit_wait));
			marked++;
			while (!waiting) {}
			waiting = false;
			for (int j = 0; j < 2000; j++)
				std::this_thread::yield();
			ch.push(static_cast<u32>(i));
			all = reaches(woken, i + 1);
			u32 out = 0;
			ch.try_pop(out);
		}
		if (!all)
		{
			std::fprintf(stderr, "%s:%d: check failed: an SPU channel waiter not woken\n", __FILE__, __LINE__);
			std::_Exit(1);
		}
		waiter.join();
		std::printf("test_emu_waits: an SPU channel: pushed and popped, 20000 races with one winner each, 300 wakes\n");
	}

	// The SPU's inbound mailbox (spu_channel_4_t), one 16-byte retro_atomic
	// word: four values in order, a fifth overwriting the last, set only
	// when empty, pushes from two threads at once all counted (20000
	// rounds), and a waiter woken by a push, 300 times
	{
		spu_channel_4_t mb{};
		u32 v = 0;
		CHECK(mb.get_count() == 0 && !mb.try_pop(v));
		for (u32 i = 1; i <= 5; i++)
			mb.push(i * 10);
		CHECK(mb.get_count() == 4);
		u32 vals[4]{};
		CHECK(mb.try_read(vals) == 4 && vals[0] == 10 && vals[1] == 20 && vals[2] == 30 && vals[3] == 50);
		for (u32 want : {10u, 20u, 30u, 50u})
			CHECK(mb.try_pop(v) && v == want);
		CHECK(mb.get_count() == 0);
		CHECK(mb.try_set_if_empty(7) && !mb.try_set_if_empty(8));
		CHECK(mb.try_pop(v) && v == 7);

		constexpr int rounds = 20000;
		std::vector<std::atomic<int>> arrived(rounds), done(rounds);
		int miscounted = 0;
		const auto pusher = [&](bool checks)
		{
#ifdef __linux__
			// Each on a CPU of its own, so that the two pushes do overlap
			cpu_set_t set;
			CPU_ZERO(&set);
			CPU_SET((checks ? 0 : 1) % std::max(1u, std::thread::hardware_concurrency()), &set);
			pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
			for (int i = 0; i < rounds; i++)
			{
				arrived[i]++;
				while (arrived[i] < 2) {}
				mb.push(checks ? 1 : 2);
				done[i]++;
				if (checks)
				{
					while (done[i] < 2) {}
					miscounted += mb.get_count() != 2;
					mb.clear();
					done[i] = 3;
				}
				else
				{
					while (done[i] < 3) {}
				}
			}
		};
		{
			std::thread a(pusher, true), b(pusher, false);
			a.join();
			b.join();
		}
		CHECK(miscounted == 0);

		std::atomic<int> woken{0}, marked{0};
		std::atomic<bool> waiting{false};
		std::thread waiter([&]
		{
			for (int i = 0; i < 300; i++)
			{
				while (marked < i + 1) {}
				const u32 seen = static_cast<u32>(retro_atomic_load_acquire_128_lo(&mb.values));
				waiting = true;
				while (static_cast<u32>(retro_atomic_load_acquire_128_lo(&mb.values)) == seen)
				{
					const retro_waitaddr_t w{&mb.values, seen};
					retro_waitaddr_wait(&w, 1, -1);
				}
				woken++;
			}
		});
		bool all = true;
		for (int i = 0; i < 300 && all; i++)
		{
			// A waiter marked, as pop_wait marks it, then a push wakes it
			mb.values_op([](spu_channel_4_t::sync_var_t& data) { data.waiting = spu_channel_4_t::bit_wait; return 0; });
			marked++;
			while (!waiting) {}
			waiting = false;
			for (int j = 0; j < 2000; j++)
				std::this_thread::yield();
			mb.push(static_cast<u32>(i));
			all = reaches(woken, i + 1);
			mb.clear();
		}
		if (!all)
		{
			std::fprintf(stderr, "%s:%d: check failed: an SPU inbound mailbox waiter not woken\n", __FILE__, __LINE__);
			std::_Exit(1);
		}
		waiter.join();
		std::printf("test_emu_waits: the SPU inbound mailbox: four values in order, 20000 races counted, 300 wakes\n");
	}

	// An SPU's interrupt control: only the mask's bits set, cleared, and
	// two threads setting different bits at once both kept (20000 rounds,
	// each thread on a CPU of its own)
	{
		spu_int_ctrl_t ctrl{};
		retro_atomic_store_release_64(&ctrl.mask, 0b0110);
		ctrl.set(0b0011);
		CHECK(ctrl.get_stat() == 0b0010);
		ctrl.clear(0b0010);
		CHECK(ctrl.get_stat() == 0);

		retro_atomic_store_release_64(&ctrl.mask, -1);
		constexpr int rounds = 20000;
		std::vector<std::atomic<int>> arrived(rounds), done(rounds);
		int lost = 0;
		const auto setter = [&](bool checks)
		{
#ifdef __linux__
			cpu_set_t set;
			CPU_ZERO(&set);
			CPU_SET((checks ? 0 : 1) % std::max(1u, std::thread::hardware_concurrency()), &set);
			pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
			for (int i = 0; i < rounds; i++)
			{
				arrived[i]++;
				while (arrived[i] < 2) {}
				ctrl.set(checks ? 1 : 2);
				done[i]++;
				if (checks)
				{
					while (done[i] < 2) {}
					lost += ctrl.get_stat() != 3;
					ctrl.clear(3);
					done[i] = 3;
				}
				else
				{
					while (done[i] < 3) {}
				}
			}
		};
		std::thread a(setter, true), b(setter, false);
		a.join();
		b.join();
		CHECK(lost == 0);
		ctrl.clear();
		std::printf("test_emu_waits: an SPU's interrupt control: masked, cleared, 20000 races with both bits kept\n");
	}

	// The SPU cache line waiters: each word on a line of its own, and two
	// threads counting themselves in on one at once, 200000 times each
	// (each on a CPU of its own), none of the counts lost
	{
		static_assert(alignof(spu_thread::spu_waiter_t) == 128 && sizeof(spu_thread::spu_waiter_t) == 128);
		retro_atomic_store_release_64(&spu_thread::g_spu_waiters_by_value[5].word, 0);
		const auto [old0, took] = spu_thread::waiter_op(5, [](u64& x) { if (x) return false; x = 0x1000; return true; });
		CHECK(old0 == 0 && took && spu_thread::waiter_load(5) == 0x1000);
		std::atomic<int> arrived{0};
		const auto counter = [&](int cpu)
		{
#ifdef __linux__
			cpu_set_t set;
			CPU_ZERO(&set);
			CPU_SET(cpu % std::max(1u, std::thread::hardware_concurrency()), &set);
			pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
			(void)cpu;
#endif
			arrived++;
			while (arrived < 2) {}
			for (int i = 0; i < 200000; i++)
				spu_thread::waiter_op(5, [](u64& x) { x += 0x1'0000'0000ull; });
		};
		std::thread a(counter, 0), b(counter, 1);
		a.join();
		b.join();
		CHECK(spu_thread::waiter_load(5) == 0x1000 + 400000 * 0x1'0000'0000ull);
		retro_atomic_store_release_64(&spu_thread::g_spu_waiters_by_value[5].word, 0);
		std::printf("test_emu_waits: the SPU cache line waiters, 400000 changes from two threads kept\n");
	}

	// A game's callback and its argument in one retro_atomic word: installed
	// only where none is, taken off whole, and of two threads installing at
	// once, one wins
	{
		sysutil_cb_word w;
		CHECK(w.cb() == 0);
		CHECK(w.install(0x10000, 0x20000));
		CHECK(!w.install(0x30000, 0x40000));
		CHECK(w.get() == std::make_pair(u32{0x10000}, u32{0x20000}));
		CHECK(w.clear());
		CHECK(!w.clear());
		w.set(0xfffffff0, 0xdeadbeef);
		CHECK(w.cb() == 0xfffffff0 && w.get().second == 0xdeadbeef);
		CHECK(w.clear());

		// Two threads that stay, meeting at each round (a thread made for
		// each round mostly comes after the other has finished)
		constexpr int rounds = 20000;
		int doubled = 0;
		std::vector<std::atomic<int>> wins(rounds), arrived(rounds), done(rounds);
		const auto installer = [&](u32 cb, u32 arg)
		{
			for (int i = 0; i < rounds; i++)
			{
				arrived[i]++;
				while (arrived[i] < 2) {}
				wins[i] += w.install(cb, arg);
				done[i]++;
				// The first thread clears the word once both are done
				if (cb == 0x1000)
				{
					while (done[i] < 2) {}
					const auto [got_cb, got_arg] = w.get();
					doubled += wins[i] != 1 || !((got_cb == 0x1000 && got_arg == 1) || (got_cb == 0x2000 && got_arg == 2));
					w.clear();
					done[i] = 3;
				}
				else
				{
					while (done[i] < 3) {}
				}
			}
		};
		std::thread a(installer, 0x1000u, 1u), b(installer, 0x2000u, 2u);
		a.join();
		b.join();
		CHECK(doubled == 0);
		std::printf("test_emu_waits: a sysutil callback word: installed only where none is, 20000 races with one winner each\n");
	}

	// A passive lock slot: its owner read back, and given up only by the
	// thread holding it
	{
		vm::passive_lock_t slot{};
		alignas(64) static char a_obj[64], b_obj[64];
		cpu_thread* const a = reinterpret_cast<cpu_thread*>(a_obj);
		cpu_thread* const b = reinterpret_cast<cpu_thread*>(b_obj);
		CHECK(vm::passive_lock_owner(&slot) == nullptr);
		CHECK(retro_atomic_cas_ptr(&slot, nullptr, static_cast<void*>(a)));
		CHECK(vm::passive_lock_owner(&slot) == a);
		CHECK(!vm::passive_lock_drop(&slot, b));
		CHECK(vm::passive_lock_owner(&slot) == a);
		CHECK(vm::passive_lock_drop(&slot, a));
		CHECK(vm::passive_lock_owner(&slot) == nullptr);
		CHECK(!vm::passive_lock_drop(&slot, a));
		std::printf("test_emu_waits: a passive lock slot: given up only by its owner\n");
	}

	// A reservation's word: where it sits in reservation memory (64 bytes
	// a cache line, the 16-byte view on the word, the byte bits after it),
	// and two threads stepping its time on at once 200000 times
	// each (each on a CPU of its own), none of the counts lost and none
	// taken while it is locked
	{
		CHECK(reinterpret_cast<u8*>(vm::reservation_word(0x10080)) == vm::g_reservations + (0x10080 & 0xff80) / 2);
		CHECK(reinterpret_cast<u8*>(vm::reservation_wide(0x100ff)) == reinterpret_cast<u8*>(vm::reservation_word(0x10080)));
		CHECK(reinterpret_cast<u8*>(vm::reservation_byte_bits(0x10080)) == reinterpret_cast<u8*>(vm::reservation_word(0x10080)) + 16);

		alignas(128) static vm::reservation_t word{};
		constexpr int rounds = 200000;
		std::atomic<int> refused{0};
		auto taker = [&](int cpu)
		{
#ifdef __linux__
			cpu_set_t set;
			CPU_ZERO(&set);
			CPU_SET(cpu % std::max(1u, std::thread::hardware_concurrency()), &set);
			pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
			for (int i = 0; i < rounds; i++)
			{
				const auto [old, ok] = vm::reservation_fetch_op(&word, [](u64& r)
				{
					if (r & vm::rsrv_unique_lock)
					{
						return false;
					}

					r += 128;
					return true;
				});

				if (!ok)
				{
					refused++;
				}
			}
		};
		std::thread a(taker, 0), b(taker, 1);
		a.join();
		b.join();
		CHECK(vm::reservation_load(&word) == 2ull * rounds * 128);
		CHECK(refused == 0);

		// Locked: refused, the word untouched
		vm::reservation_release(&word, vm::rsrv_unique_lock);
		CHECK(!vm::reservation_fetch_op(&word, [](u64& r) { if (r & vm::rsrv_unique_lock) return false; r += 1; return true; }).second);
		CHECK(vm::reservation_load(&word) == vm::rsrv_unique_lock);
		CHECK(vm::reservation_try_lock(&word, 0) == false);
		vm::reservation_release(&word, 128);
		CHECK(vm::reservation_try_lock(&word, 128));
		CHECK(vm::reservation_load(&word) == (128 | vm::rsrv_unique_lock));
		CHECK(vm::reservation_add(&word, 0 - vm::rsrv_unique_lock + 128) == (128 | vm::rsrv_unique_lock));
		CHECK(vm::reservation_load(&word) == 256);
		std::printf("test_emu_waits: a reservation's word: in place, and 400000 steps from two threads none lost\n");
	}

	// A reservation waiter slot: a waiter makes the flag odd and counts
	// itself in, a notify makes it even once (a second finds none to
	// wake), and a thread waiting on it is woken by the notify long
	// before its 3 s bound
	{
		const u32 addr = 0x20080;
		const u64 rtime = vm::reservation_time(addr) & -128;
		auto [waiter, flag] = vm::reservation_notifier_begin_wait(addr, rtime);
		CHECK(waiter != nullptr && flag % 2 == 1);
		CHECK(vm::reservation_notifier_count(addr, rtime) == 1);
		CHECK(vm::reservation_notifier_notify(addr, rtime, true) == vm::reservation_waiter_flag(waiter));
		CHECK(vm::reservation_notifier_count(addr, rtime) == 0);
		CHECK(vm::reservation_notifier_notify(addr, rtime, true) == nullptr);
		vm::reservation_notifier_end_wait(*waiter);
		CHECK(vm::reservation_waiter_load(waiter) >> 32 == 0);

		std::atomic<int> waiting{0};
		std::atomic<s64> waited_ms{-1};
		std::thread t([&]
		{
			auto [w, f] = vm::reservation_notifier_begin_wait(addr, rtime);
			waiting = 1;
			const auto t0 = std::chrono::steady_clock::now();
			if (w)
			{
				while (static_cast<u32>(vm::reservation_waiter_load(w)) == f && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3))
				{
					vm::reservation_notifier_wait(w, f, 3'000'000'000);
				}
				vm::reservation_notifier_end_wait(*w);
			}
			waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		});
		while (!waiting) {}
		// Let it park, then notify
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		vm::reservation_notifier_notify(addr, rtime);
		t.join();
		CHECK(waited_ms >= 0 && waited_ms < 1000);
		std::printf("test_emu_waits: a reservation waiter slot: counted, notified once, waiter woken in %lld ms\n", static_cast<long long>(waited_ms.load()));
	}

	std::printf("test_emu_waits: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
