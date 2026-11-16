// An lv2 event queue's events (Emu/Cell/lv2/sys_event.h): a ring made with
// the queue (lv2_evq.c), where there was a std::deque that took a block from
// the heap for every sixteen events sent and gave one back for every
// sixteen taken.
//
// - events come out in the order they went in, round the ring many times;
// - the queue holds as many as its size and refuses the next (EBUSY);
// - sending and taking allocate nothing: 100000 events through a queue;
// - a queue drained is empty, and takes events again;
// - a savestate has the queue as it had it with the deque, byte for byte,
//   and a queue read back from one has the same events.
//
// Built by `make check` from the core's own objects and archives, the same
// link as the core's with this main.

#include "stdafx.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/lv2/sys_event.h"
#include "util/serialization.hpp"

#include <cstdio>
#include <thread>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <new>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static thread_local bool t_counting = false;
static thread_local usz t_allocations = 0;

void* operator new(std::size_t n)
{
	if (t_counting)
		t_allocations++;
	if (void* p = std::malloc(n ? n : 1))
		return p;
	throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static lv2_event event_of(u64 n)
{
	return std::make_tuple(n, n * 3 + 1, ~n, n << 20);
}

// As sys_event_queue_receive takes one: no turn
static bool take(lv2_event_queue& queue, lv2_event& out)
{
	u64 ev[4];
	if (!lv2_evq_try(&queue.q, ev))
		return false;
	out = std::make_tuple(ev[0], ev[1], ev[2], ev[3]);
	return true;
}

// The events as they stand, in order
static std::vector<lv2_event> events_of(lv2_event_queue& queue)
{
	std::vector<lv2_event> out;
	lv2_evq_each(&queue.q, [](void* ctx, const uint64_t ev[4])
	{
		static_cast<std::vector<lv2_event>*>(ctx)->emplace_back(ev[0], ev[1], ev[2], ev[3]);
	}, &out);
	return out;
}

int main()
{
	g_fxo->reset();
	g_fxo->init<id_manager::id_map<lv2_obj>>();

	const u32 id = idm::make<lv2_obj, lv2_event_queue>(SYS_SYNC_PRIORITY, SYS_PPU_QUEUE, 127, 0x1234, 0);
	const auto queue = idm::get_unlocked<lv2_obj, lv2_event_queue>(id);
	CHECK(!!queue);
	retro_atomic_fetch_add_int(&queue->exists, 1); // as sys_event_queue_create leaves it

	// As many as its size, and no more
	for (u64 n = 0; n < 127; n++)
		CHECK(queue->send(event_of(n)) == CellError{});
	CHECK(lv2_evq_count(&queue->q) == 127);
	CHECK(queue->send(event_of(999)) == CELL_EBUSY);
	CHECK(lv2_evq_count(&queue->q) == 127);

	// In the order they went in
	lv2_event got;
	for (u64 n = 0; n < 127; n++)
	{
		CHECK(take(*queue, got) && got == event_of(n));
	}
	CHECK(!take(*queue, got));

	// 100000 events through it, a few waiting at a time, round the ring:
	// in order, and nothing allocated
	{
		u64 sent = 0, taken = 0, wrong = 0;
		t_allocations = 0;
		t_counting = true;
		while (taken < 100000)
		{
			const u64 burst = 1 + sent % 37;
			for (u64 i = 0; i < burst && lv2_evq_count(&queue->q) < 127; i++)
				if (queue->send(event_of(sent)) == CellError{})
					sent++;
			const u64 back = 1 + taken % 29;
			for (u64 i = 0; i < back && take(*queue, got); i++)
			{
				wrong += got != event_of(taken);
				taken++;
			}
		}
		t_counting = false;
		CHECK(wrong == 0);
		CHECK(t_allocations == 0);
		std::printf("test_lv2_events: %llu events through a queue, %llu out of order, %llu allocations\n",
			static_cast<unsigned long long>(taken), static_cast<unsigned long long>(wrong), static_cast<unsigned long long>(t_allocations));

		while (take(*queue, got))
			taken++;
		CHECK(taken == sent);
	}

	// Sent from two threads and taken from two others at once, as PPU and SPU
	// threads and the timer thread send while handlers take: each send and
	// take in the queue's turn, so every event comes out exactly once, and in
	// the order each sender sent its own
	{
		static constexpr u64 per_sender = 50000;
		static retro_atomic_int_t s_taken_count, s_seen[2 * per_sender], s_order_bad, s_sent_all;
		retro_atomic_store_release_int(&s_taken_count, 0);
		retro_atomic_store_release_int(&s_order_bad, 0);
		retro_atomic_store_release_int(&s_sent_all, 0);

		std::thread threads[4];
		for (u32 t = 0; t < 2; t++)
		{
			threads[t] = std::thread([t, &queue]
			{
				// A queue that stays full that long has lost count: given up on,
				// and what was not sent is missed
				for (u64 i = 0, full = 0; i < per_sender && full < 2000000;)
				{
					// The sender in the first word, its count in the second
					if (queue->send(std::make_tuple(u64{t}, i, u64{0}, u64{0})) == CellError{})
						i++, full = 0;
					else
						full++, std::this_thread::yield();
				}
			});
		}
		for (u32 t = 2; t < 4; t++)
		{
			threads[t] = std::thread([&queue]
			{
				u64 last[2] = {0, 0};
				bool any[2] = {false, false};
				lv2_event e;

				while (retro_atomic_load_acquire_int(&s_taken_count) < static_cast<int>(2 * per_sender))
				{
					if (!take(*queue, e))
					{
						// All sent and none left: one lost is missed, not waited for
						if (retro_atomic_load_acquire_int(&s_sent_all) && !lv2_evq_count(&queue->q))
							break;
						std::this_thread::yield();
						continue;
					}

					const u64 from = std::get<0>(e), n = std::get<1>(e);
					if (from > 1 || n >= per_sender || (any[from] && n <= last[from]))
						retro_atomic_fetch_add_int(&s_order_bad, 1);
					else
						retro_atomic_fetch_add_int(&s_seen[from * per_sender + n], 1);
					last[from & 1] = n;
					any[from & 1] = true;
					retro_atomic_fetch_add_int(&s_taken_count, 1);
				}
			});
		}
		threads[0].join();
		threads[1].join();
		retro_atomic_store_release_int(&s_sent_all, 1);
		threads[2].join();
		threads[3].join();

		u64 twice = 0, missed = 0;
		for (u64 i = 0; i < 2 * per_sender; i++)
		{
			const int seen = retro_atomic_load_acquire_int(&s_seen[i]);
			twice += seen > 1;
			missed += seen == 0;
		}

		CHECK(twice == 0 && missed == 0 && retro_atomic_load_acquire_int(&s_order_bad) == 0);
		std::printf("test_lv2_events: 2 threads sending, 2 taking: %llu events, %llu twice, %llu missed, %d out of order\n",
			static_cast<unsigned long long>(2 * per_sender), static_cast<unsigned long long>(twice), static_cast<unsigned long long>(missed),
			retro_atomic_load_acquire_int(&s_order_bad));
	}

	// Drained (sys_event_queue_drain), then used again
	for (u64 n = 0; n < 50; n++)
		CHECK(queue->send(event_of(n)) == CellError{});
	lv2_evq_drain(&queue->q);
	CHECK(!lv2_evq_count(&queue->q) && !take(*queue, got));
	for (u64 n = 500; n < 540; n++)
		CHECK(queue->send(event_of(n)) == CellError{});

	// A savestate: the bytes a std::deque of the same events made
	{
		utils::serial saved;
		queue->save(saved);

		std::deque<lv2_event> as_deque;
		for (u64 n = 500; n < 540; n++)
			as_deque.emplace_back(event_of(n));
		utils::serial expected;
		expected(queue->protocol, queue->type, queue->size, queue->name, queue->key, as_deque);

		CHECK(saved.data.size() == expected.data.size());
		CHECK(saved.data == expected.data);

		// And read back: a queue made from the savestate has the same events
		utils::serial in;
		in.set_reading_state(std::move(saved.data));
		lv2_event_queue back{stx::exact_t<utils::serial&>(in)};
		CHECK(back.protocol == queue->protocol && back.type == queue->type && back.size == queue->size);
		CHECK(back.name == queue->name && back.key == queue->key);
		const auto back_events = events_of(back);
		CHECK(back_events.size() == 40);
		u64 wrong = 0;
		for (u64 n = 0; n < back_events.size(); n++)
			wrong += back_events[n] != event_of(500 + n);
		CHECK(wrong == 0);
	}

	// A queue of one event: it holds one, not the two its block would
	{
		const u32 small_id = idm::make<lv2_obj, lv2_event_queue>(SYS_SYNC_PRIORITY, SYS_PPU_QUEUE, 1, 0, 0);
		const auto small = idm::get_unlocked<lv2_obj, lv2_event_queue>(small_id);
		retro_atomic_fetch_add_int(&small->exists, 1);
		CHECK(small->send(event_of(7)) == CellError{});
		CHECK(small->send(event_of(8)) == CELL_EBUSY);
		CHECK(take(*small, got) && got == event_of(7));
		CHECK(small->send(event_of(8)) == CellError{});
		CHECK(take(*small, got) && got == event_of(8) && !take(*small, got));
	}

	std::printf("test_lv2_events: %s\n", failures ? "FAIL" : "ok");
	std::fflush(stdout);
	// The emulator's object map wants an emulator's shutdown; there is none here
	std::_Exit(failures ? 1 : 0);
}
