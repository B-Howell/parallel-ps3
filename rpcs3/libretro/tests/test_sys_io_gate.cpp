// A pad connected or disconnected on its way to the game (sys_io's event
// queue, Emu/Cell/Modules/sys_io_.cpp): retro_run sends it, and must not
// wait for the game's threads to do so.
//
// - sys_config_stop holds sys_io's mutex while it waits for the game's
//   handler thread to end. A send from the frontend's thread meanwhile
//   returns, with the event in the queue: it does not wait for that mutex.
// - Not started, or stopped: nothing is sent, and nothing waits.
// - sys_config_stop does not wait for a send under way: it closes the gate
//   and goes on, and the send, holding the queue it found, sends nothing.
//   (The gate was a count of sends under way, which the stop spun on.)
// - A send never reaches a queue made since under the same id: a slow
//   thread sending and one stopping and starting, the queue made again
//   under the same id each time, 200000 times over.
//
// The module is compiled into this test (its state is private to its source
// file); the rest is the core's own objects and archives, the same link as
// the core's with this main. Built by `make check`.

#include "../../Emu/Cell/Modules/sys_io_.cpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main()
{
	g_fxo->reset();
	g_fxo->init<id_manager::id_map<lv2_obj>>();
	g_fxo->init<libio_sys_config>();
	auto& cfg = g_fxo->get<libio_sys_config>();

	const u32 queue_id = idm::make<lv2_obj, lv2_event_queue>(SYS_SYNC_PRIORITY, SYS_PPU_QUEUE, 0x20, 0, 0);
	CHECK(queue_id != 0);
	const auto queue = idm::get_unlocked<lv2_obj, lv2_event_queue>(queue_id);
	CHECK(!!queue);
	retro_atomic_fetch_add_int(&queue->exists, 1); // as sys_event_queue_create leaves it

	// Not started: nothing sent
	cfg.queue_id = queue_id;
	send_sys_io_connect_event(1, 1);
	CHECK(lv2_events_empty(&queue->events));

	// Started, as sys_config_start leaves it
	cfg.init_ctr = 1;
	sys_io_gate_open(&cfg.gate, queue_id);

	// A game's thread in sys_config_stop, waiting for the handler thread
	// with the mutex held: retro_run's send goes through
	{
		std::atomic<bool> held{false}, release{false}, sent{false};
		std::thread game([&]
		{
			std::lock_guard lock(cfg.mtx);
			held = true;
			while (!release)
				std::this_thread::yield();
		});

		while (!held)
			std::this_thread::yield();

		std::thread run([&]
		{
			send_sys_io_connect_event(2, 1);
			sent = true;
		});

		for (int i = 0; i < 20'000'000 && !sent; i++)
			std::this_thread::yield();

		CHECK(sent);
		release = true;
		run.join();
		game.join();

		CHECK(lv2_events_count(&queue->events) == 1);
		if (!lv2_events_empty(&queue->events))
		{
			const auto [source, d1, d2, d3] = *lv2_events_front(&queue->events);
			CHECK(source == 0 && d1 == 1 && d2 == 2 && d3 == 1);
		}
	}

	// Stopped: nothing is sent
	sys_io_gate_close(&cfg.gate);
	send_sys_io_connect_event(3, 1);
	CHECK(lv2_events_count(&queue->events) == 1);

	// A send under way (it has read the gate and holds the queue) does not
	// hold a stop back, and sends nothing after it
	{
		sys_io_gate_open(&cfg.gate, queue_id);
		const u64 seen = sys_io_gate_word(&cfg.gate);
		CHECK((seen & 1) && static_cast<u32>(seen >> 32) == queue_id);
		const auto t0 = std::chrono::steady_clock::now();
		sys_io_gate_close(&cfg.gate);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		CHECK(ms < 1.0);
		CHECK(sys_io_gate_word(&cfg.gate) != seen);
		// Started again under the same id: still not the queue that was open
		sys_io_gate_open(&cfg.gate, queue_id);
		CHECK(sys_io_gate_word(&cfg.gate) != seen);
		sys_io_gate_close(&cfg.gate);
	}

	// One thread sending, slowly, and one stopping and starting, the queue
	// made again under the same id each time: a send that goes through has
	// the queue that was open when it read the gate
	{
		constexpr int cycles = 200000;
		std::vector<std::atomic<u32>> made_at(cycles + 2);
		std::atomic<u32> made{0};
		std::atomic<bool> stop{false};
		std::atomic<u32> wrong{0}, sends{0};
		made_at[0] = 0;
		sys_io_gate_open(&cfg.gate, queue_id);
		const u64 first = sys_io_gate_word(&cfg.gate);

		std::thread run([&]
		{
			u32 n = 0;
			while (!stop)
			{
				const u64 seen = sys_io_gate_word(&cfg.gate);
				if (!(seen & 1))
					continue;
				// Slow to take hold of the queue
				if ((++n & 7) == 0)
					std::this_thread::yield();
				const u32 held = made.load(std::memory_order_acquire);
				if (sys_io_gate_word(&cfg.gate) == seen)
				{
					sends++;
					const u32 opened = static_cast<u32>(((seen & 0xffff'fffeull) - (first & 0xffff'fffeull)) / 4);
					if (opened > static_cast<u32>(cycles) || held != made_at[opened].load(std::memory_order_acquire))
						wrong++;
				}
			}
		});

		for (int i = 1; i <= cycles; i++)
		{
			sys_io_gate_close(&cfg.gate);
			// The queue destroyed, and made again under the same id
			made.fetch_add(1, std::memory_order_release);
			made_at[i].store(made.load(), std::memory_order_release);
			sys_io_gate_open(&cfg.gate, queue_id);
			if ((i & 0xff) == 0)
				std::this_thread::yield();
		}

		stop = true;
		run.join();
		CHECK(wrong == 0);
		CHECK(sends > 0);
		std::printf("test_sys_io_gate: 200000 stops and starts against %u sends, %u sends to a queue made since\n", sends.load(), wrong.load());
	}

	std::printf("test_sys_io_gate: %s\n", failures ? "FAIL" : "ok");
	std::fflush(stdout);
	std::_Exit(failures ? 1 : 0);
}
