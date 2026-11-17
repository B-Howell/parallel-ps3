// The ID manager with no lock over the IDs (Emu/IdManager.h): IDs given out
// by a compare-and-swap on their keys, taken away by the one thread that
// holds the ID's deciding mark.
//
// - IDs given out up to the count, none twice, and a freed one given again
//   with its invalidation count moved on;
// - four threads making, finding and removing IDs at once while a fifth
//   walks them all (select): every thread finds its own object under its
//   ID, none is given an ID another holds;
// - two threads removing the same ID at once: one succeeds;
// - two threads withdrawing the same ID with a question at once: never
//   both asking, one takes it;
// - a close keeps a withdrawal waiting until it ends;
// - a stop waits for no ID being made (wait_uncreating) and returns.
//
// Built by `make check` from the core's own objects and archives, the same
// link as the core's with this main.

#include "stdafx.h"
#include "Emu/IdManager.h"

#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

struct thing final
{
	static const u32 id_base = 0x10000000;
	static const u32 id_step = 0x100;
	static const u32 id_count = 64;
	static constexpr std::pair<u32, u32> id_invl_range = {0, 8};
	static constexpr double savestate_init_pos = 0;

	std::atomic<u32> owner{0};
	std::atomic<int> asking{0};
};

static void basics()
{
	g_fxo->reset();
	g_fxo->init<id_manager::id_map<thing>>();

	std::vector<u32> ids;

	for (u32 i = 0; i < thing::id_count; i++)
	{
		const u32 id = idm::make<thing>();
		CHECK(id && idm::check_unlocked<thing>(id));

		for (const u32 other : ids)
		{
			CHECK(other != id);
		}

		ids.push_back(id);
	}

	// Out of IDs
	CHECK(!idm::make<thing>());

	// Freed and given again: the same place, another ID
	const u32 old = ids[5];
	CHECK(idm::remove<thing>(old) && !idm::remove<thing>(old) && !idm::check_unlocked<thing>(old));
	const u32 again = idm::make<thing>();
	CHECK(again && again != old && (again & ~0xffu) == (old & ~0xffu) && !idm::check_unlocked<thing>(old));

	// Withdrawn after a question, or kept when it says no
	const auto kept = idm::withdraw<thing>(again, [](thing&) -> int { return 1; });
	CHECK(kept && kept.ret == 1 && idm::check_unlocked<thing>(again));
	const auto taken = idm::withdraw<thing>(again, [](thing&) -> int { return 0; });
	CHECK(taken && !taken.ret && !idm::check_unlocked<thing>(again));

	u32 seen = 0;
	idm::select<thing>([&](u32, thing&) { seen++; });
	CHECK(seen == thing::id_count - 1);

	idm::clear<thing>();
	seen = 0;
	idm::select<thing>([&](u32, thing&) { seen++; });
	CHECK(seen == 0);
	id_manager::wait_uncreating();
}

static void together()
{
	g_fxo->reset();
	g_fxo->init<id_manager::id_map<thing>>();

	constexpr int threads = 4;
	constexpr int rounds = 20000;
	std::atomic<int> bad{0}, made{0}, walking{1};

	std::thread walker([&]
	{
		while (walking.load())
		{
			idm::select<thing>([&](u32 id, thing& t)
			{
				if (!t.owner.load() && idm::check_unlocked<thing>(id) == &t)
				{
					// Found before it was owned: fine; never another's
				}
			});
		}
	});

	std::vector<std::thread> workers;

	for (int w = 0; w < threads; w++)
	{
		workers.emplace_back([&, w]
		{
			for (int r = 0; r < rounds; r++)
			{
				const auto obj = idm::make_ptr<thing>();

				if (!obj)
				{
					continue;
				}

				const u32 id = idm::last_id();

				// Its own, under its ID: no other thread was given it
				if (obj->owner.exchange(static_cast<u32>(w + 1)) != 0)
				{
					bad++;
				}

				const auto found = idm::get_unlocked<thing>(id);

				if (found.get() != obj.get() || found->owner.load() != static_cast<u32>(w + 1))
				{
					bad++;
				}

				made++;

				if (!idm::remove<thing>(id))
				{
					bad++;
				}
			}
		});
	}

	for (auto& t : workers)
	{
		t.join();
	}

	walking = 0;
	walker.join();

	CHECK(bad.load() == 0);
	CHECK(made.load() > 0);

	// Two removing one ID at once: one of them removes it
	int doubles = 0, none = 0;

	for (int r = 0; r < 5000; r++)
	{
		const u32 id = idm::make<thing>();
		std::atomic<int> go{0}, ok{0};

		auto remover = [&]
		{
			while (!go.load()) {}
			if (idm::remove<thing>(id)) ok++;
		};

		std::thread a(remover), b(remover);
		go = 1;
		a.join();
		b.join();

		doubles += ok.load() > 1;
		none += ok.load() == 0;
	}

	CHECK(doubles == 0 && none == 0);

	// Two withdrawing one ID with a question at once: never both asking
	int both_asking = 0, both_took = 0;

	for (int r = 0; r < 5000; r++)
	{
		const u32 id = idm::make<thing>();
		std::atomic<int> go{0}, took{0};

		auto withdrawer = [&]
		{
			while (!go.load()) {}

			const auto res = idm::withdraw<thing>(id, [&](thing& t) -> int
			{
				if (t.asking.fetch_add(1) != 0)
				{
					both_asking++;
				}

				std::this_thread::yield();
				t.asking.fetch_sub(1);
				return 0;
			});

			if (res && !res.ret)
			{
				took++;
			}
		};

		std::thread a(withdrawer), b(withdrawer);
		go = 1;
		a.join();
		b.join();

		both_took += took.load() != 1;
	}

	CHECK(both_asking == 0 && both_took == 0);

	// A close keeps a withdrawal waiting until it ends
	{
		const u32 id = idm::make<thing>();
		std::atomic<int> withdrawn{0};
		std::thread t;

		{
			const auto closed = idm::close<thing>(id);

			t = std::thread([&]
			{
				if (idm::withdraw<thing>(id))
				{
					withdrawn = 1;
				}
			});

			for (int i = 0; i < 2000; i++)
			{
				std::this_thread::yield();
			}

			CHECK(!withdrawn.load() && idm::check_unlocked<thing>(id));
		}

		t.join();
		CHECK(withdrawn.load() && !idm::check_unlocked<thing>(id));
	}

	idm::clear<thing>();
	id_manager::wait_uncreating();

	std::printf("test_idm: %d threads making and removing IDs, a walker, %d made: none given twice; one remover, one asker\n", threads, made.load());
}

int main()
{
	basics();
	together();
	g_fxo->clear();

	if (failures)
	{
		std::fprintf(stderr, "test_idm: %d failed\n", failures);
		return 1;
	}

	std::printf("test_idm: IDs given out with no lock, freed and given again, withdrawn by one\n");
	return 0;
}
