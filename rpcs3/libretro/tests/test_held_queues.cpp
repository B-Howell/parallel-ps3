// Queues whose elements were not plain data, on libretro-common's
// retro_deque (queues/retro_deque.h):
//
// - the timer thread's list of timers (Emu/Cell/lv2/sys_timer.cpp), each a
//   reference held as plain bytes: a timer added is held once and found,
//   one taken out of the middle is let go and the rest keep their order,
//   against the std::deque of shared_ptr it was; the list is made for every
//   timer there can be and stays that size; ending the thread lets go of
//   all it still holds;
// - cellVoice's queues to notify (Emu/Cell/Modules/cellVoice.h), one list
//   where there was a map of lists: in a savestate the bytes the map made,
//   read back by the map's own code, and the map's bytes read back here.
//
// - the DNS hook's made-up replies (Emu/NP/np_dnshook.cpp), through its
//   own code: each socket reads its own replies in order, a socket no
//   longer watched takes its unread replies with it.
//
// - the overlay's messages (Emu/RSX/Overlays/overlay_message.cpp), through
//   its own code: queued, shown three at a time, let go when done with,
//   and freed with the overlay - every message accounted for.
//
// sys_timer.cpp is compiled into this test (the timer thread is private to
// it); the rest is the core's own objects and archives, the same link as
// the core's with this main. Built by `make check`.

#include "../../Emu/Cell/lv2/sys_timer.cpp"
#include "../../Emu/Cell/lv2/sys_usbd.cpp"
#include "../../Emu/Cell/lv2/sys_spu.cpp"
#include "Emu/Cell/Modules/cellVoice.h"
#include "Emu/NP/np_dnshook.h"
#include "Emu/RSX/Overlays/overlay_message.h"
#include "Emu/emu_callbacks.h"
#include "Emu/system_config.h"
#include "util/serialization.hpp"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>
#include <unordered_map>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static u32 s_rand = 0x9E3779B9;

static u32 rnd()
{
	s_rand ^= s_rand << 13;
	s_rand ^= s_rand >> 17;
	s_rand ^= s_rand << 5;
	return s_rand;
}

static bool same(const lv2_timer_thread& thread, const std::deque<shared_ptr<lv2_timer>>& model)
{
	if (lv2_timers_count(&thread.timers) != model.size())
		return false;
	for (usz i = 0; i < model.size(); i++)
		if (timer_of(lv2_timers_at(&thread.timers, i)).get() != model[i].get())
			return false;
	return true;
}

int main()
{
	g_fxo->reset();
	g_fxo->init<id_manager::id_map<lv2_obj>>();

	// The timer thread's list
	{
		std::vector<shared_ptr<lv2_timer>> made;
		for (u32 i = 0; i < 64; i++)
		{
			made.emplace_back(idm::make_ptr<lv2_obj, lv2_timer>());
			CHECK(!!made.back());
		}
		const usz held_before = made[0].use_count();

		{
			lv2_timer_thread thread;
			std::deque<shared_ptr<lv2_timer>> model;
			CHECK(lv2_timers_limit(&thread.timers) == lv2_timer::id_count);

			// Added: held once, found where it went
			thread.add(shared_ptr<lv2_timer>(made[0]));
			model.emplace_back(made[0]);
			CHECK(made[0].use_count() == held_before + 2); // the list's and the model's
			CHECK(thread.find(made[0].get()) == 0 && thread.find(made[1].get()) == 1);

			// Taken out again: let go
			timer_drop(lv2_timers_at(&thread.timers, 0));
			lv2_timers_erase(&thread.timers, 0);
			model.clear();
			CHECK(made[0].use_count() == held_before);
			CHECK(thread.find(made[0].get()) == 0 && lv2_timers_empty(&thread.timers));

			// Created and destroyed in any order, as the syscalls do it
			for (u32 step = 0; step < 200000; step++)
			{
				const shared_ptr<lv2_timer>& timer = made[rnd() % made.size()];
				const usz at = thread.find(timer.get());
				const auto it = std::find(model.begin(), model.end(), timer);

				CHECK((at == lv2_timers_count(&thread.timers)) == (it == model.end()));

				if (it == model.end())
				{
					thread.add(shared_ptr<lv2_timer>(timer));
					model.emplace_back(timer);
				}
				else
				{
					CHECK(at == static_cast<usz>(it - model.begin()));
					timer_drop(lv2_timers_at(&thread.timers, at));
					lv2_timers_erase(&thread.timers, at);
					model.erase(it);
				}

				if (!same(thread, model))
				{
					CHECK(!"the list and the std::deque differ");
					break;
				}
			}

			// Each is held once for being in the list, and not otherwise
			usz wrong = 0;
			for (const auto& timer : made)
			{
				const bool in = thread.find(timer.get()) != lv2_timers_count(&thread.timers);
				wrong += timer.use_count() != held_before + (in ? 2 : 0);
			}
			CHECK(wrong == 0);
			CHECK(lv2_timers_limit(&thread.timers) == lv2_timer::id_count);

			model.clear();
			CHECK(!lv2_timers_empty(&thread.timers));
		}

		// The thread ended: everything it held is let go
		usz wrong = 0;
		for (const auto& timer : made)
			wrong += timer.use_count() != held_before;
		CHECK(wrong == 0);
	}

	// Timers handed over to the thread from four threads at once, as
	// sys_timer_create and sys_timer_destroy on several PPU threads while
	// the thread takes them up: the list ends with exactly the timers not
	// destroyed, each held once, and nothing destroyed is held - also one
	// destroyed before its create handed it over
	{
		static lv2_timer_thread* s_thread;
		lv2_timer_thread thread;
		s_thread = &thread;
		static retro_atomic_int_t s_done;
		std::vector<shared_ptr<lv2_timer>> made[4];
		std::vector<bool> kept[4];
		static retro_atomic_int_t s_base{-1};

		std::thread takers([]
		{
			while (!retro_atomic_load_acquire_int(&s_done))
				s_thread->take_up();
		});

		std::thread makers[4];
		for (u32 t = 0; t < 4; t++)
		{
			makers[t] = std::thread([t, &made, &kept]
			{
				u32 x = t * 2654435761u + 3;
				for (u32 n = 0; n < 500; n++)
				{
					x = x * 1664525u + 1013904223u;
					shared_ptr<lv2_timer> timer = idm::make_ptr<lv2_obj, lv2_timer>();
					made[t].emplace_back(timer);
					if (n == 0 && t == 0)
						retro_atomic_store_release_int(&s_base, static_cast<int>(timer.use_count()) - 1); // less the local one
					const u32 how = (x >> 16) % 3;

					if (how == 2)
					{
						// Destroyed before the create's hand-over
						retro_atomic_fetch_sub_int(&timer->exists, 1);
						s_thread->hand_over_drop(timer.get());
						s_thread->hand_over_add(timer);
						kept[t].push_back(false);
						continue;
					}

					s_thread->hand_over_add(timer);

					if (how == 1)
					{
						retro_atomic_fetch_sub_int(&timer->exists, 1);
						s_thread->hand_over_drop(timer.get());
					}

					kept[t].push_back(how == 0);
				}
			});
		}

		for (auto& m : makers)
			m.join();
		retro_atomic_store_release_int(&s_done, 1);
		takers.join();
		thread.take_up();

		usz wrong = 0, in_list = 0, want = 0;
		for (u32 t = 0; t < 4; t++)
		{
			for (usz i = 0; i < made[t].size(); i++)
			{
				const bool in = thread.find(made[t][i].get()) != lv2_timers_count(&thread.timers);
				in_list += in;
				want += kept[t][i];
				wrong += in != kept[t][i];
				wrong += made[t][i].use_count() != static_cast<usz>(retro_atomic_load_acquire_int(&s_base)) + kept[t][i]; // and the list's when kept
			}
		}
		CHECK(wrong == 0);
		CHECK(in_list == want && lv2_timers_count(&thread.timers) == want);
		std::printf("test_held_queues: timers handed over from 4 threads: %zu kept of 2000, %zu wrong\n", in_list, wrong);

	}

	// sys_usbd's transfers, from four threads at once as from several PPU
	// threads: no transfer is given to two at once, and a status read while
	// the USB thread writes it is never half old, half new
	{
		static usb_handler_thread usbh;
		static retro_atomic_int_t s_owner[MAX_SYS_USBD_TRANSFERS], s_bad7, s_taken7, s_stop7, s_torn7;

		std::thread takers[4];
		for (u32 t = 0; t < 4; t++)
		{
			takers[t] = std::thread([t]
			{
				// Each thread keeps 16 at a time: 64 of the 68 busy, so the
				// claims meet on the few free ones
				std::pair<u32, UsbTransfer*> held[16];
				const auto take = [t](std::pair<u32, UsbTransfer*>& h)
				{
					auto&& [id, transfer] = usbh.get_free_transfer();
					h = {id, &transfer};
					if (retro_atomic_exchange_int(&s_owner[id], static_cast<int>(t + 1)))
						retro_atomic_fetch_add_int(&s_bad7, 1);
				};
				const auto complete = [](std::pair<u32, UsbTransfer*>& h)
				{
					// As the USB thread does
					retro_atomic_store_release_int(&s_owner[h.first], 0);
					retro_atomic_store_release_int(&h.second->busy, 0);
				};

				for (auto& h : held)
					take(h);

				for (int n = 0; n < 50000; n++)
				{
					complete(held[n % 16]);
					take(held[n % 16]);
					retro_atomic_fetch_add_int(&s_taken7, 1);
				}

				for (auto& h : held)
					complete(h);
			});
		}

		for (auto& t : takers)
			t.join();

		// One transfer's status written over and over, read meanwhile
		auto&& [id, transfer] = usbh.get_free_transfer();
		std::thread writer([&transfer = transfer]
		{
			for (u32 k = 1; !retro_atomic_load_acquire_int(&s_stop7); k++)
			{
				retro_seqlock_write_begin(&transfer.status_seq);
				transfer.result = static_cast<s32>(k);
				for (int spin = 0; spin < 4; spin++)
					(void)retro_atomic_load_relaxed_int(&s_stop7);
				transfer.count = k;
				retro_seqlock_write_end(&transfer.status_seq);
			}
		});

		for (int n = 0; n < 2000000; n++)
		{
			const auto [result, count] = usbh.get_transfer_status(id);
			if (result != count)
				retro_atomic_fetch_add_int(&s_torn7, 1);
		}

		retro_atomic_store_release_int(&s_stop7, 1);
		writer.join();

		CHECK(retro_atomic_load_acquire_int(&s_bad7) == 0);
		CHECK(retro_atomic_load_acquire_int(&s_taken7) == 200000);
		CHECK(retro_atomic_load_acquire_int(&s_torn7) == 0);
		std::printf("test_held_queues: USB transfers: %d taken by 4 threads, %d given to two at once; %d statuses torn\n",
			retro_atomic_load_acquire_int(&s_taken7), retro_atomic_load_acquire_int(&s_bad7), retro_atomic_load_acquire_int(&s_torn7));
	}

	// sys_usbd's calls on the USB thread (on_thread), from four threads at
	// once as from several PPU threads and the pad thread: each runs on the
	// USB thread, one at a time, and returns what it gave there; after the
	// thread stops taking calls, they run where they are made
	{
		static usb_handler_thread usbh2;
		static retro_atomic_int_t s_stop8, s_elsewhere8, s_wrong8;
		static u32 s_plain8; // not atomic: only one thread at a time may add
		static std::thread::id s_usb_id;

		std::thread usb([]
		{
			usb_handler_thread::on_usb_thread = true;
			s_usb_id = std::this_thread::get_id();
			while (!retro_atomic_load_acquire_int(&s_stop8))
			{
				const int wake = retro_atomic_load_acquire_int(&usbh2.wake_word);
				usbh2.take_requests();
				const retro_waitaddr_t w{&usbh2.wake_word, static_cast<u32>(wake)};
				retro_waitaddr_wait(&w, 1, 1000);
			}
			retro_atomic_fetch_or_int(&usbh2.requesting, usb_handler_thread::requests_closed);
			while (retro_atomic_load_acquire_int(&usbh2.requesting) & ~usb_handler_thread::requests_closed)
				usbh2.take_requests();
			usbh2.take_requests();
		});

		while (s_usb_id == std::thread::id{})
			std::this_thread::yield();

		std::thread callers[4];
		for (u32 t = 0; t < 4; t++)
		{
			callers[t] = std::thread([t]
			{
				for (u32 n = 0; n < 20000; n++)
				{
					const error_code got = usbh2.on_thread([&]() -> error_code
					{
						if (std::this_thread::get_id() != s_usb_id)
							retro_atomic_fetch_add_int(&s_elsewhere8, 1);
						const u32 was = s_plain8;
						for (int spin = 0; spin < 8; spin++)
							(void)retro_atomic_load_relaxed_int(&s_stop8);
						s_plain8 = was + 1;
						return not_an_error(t * 100000 + n);
					});

					if (static_cast<u32>(+got) != t * 100000 + n)
						retro_atomic_fetch_add_int(&s_wrong8, 1);
				}
			});
		}

		for (auto& c : callers)
			c.join();
		retro_atomic_store_release_int(&s_stop8, 1);
		usbh2.wake();
		usb.join();

		// Stopped: a call runs where it is made
		bool here = false;
		usbh2.on_thread([&]() { here = std::this_thread::get_id() != s_usb_id; });

		CHECK(s_plain8 == 80000);
		CHECK(retro_atomic_load_acquire_int(&s_elsewhere8) == 0);
		CHECK(retro_atomic_load_acquire_int(&s_wrong8) == 0);
		CHECK(here);
		std::printf("test_held_queues: USB calls: %u of 80000 from 4 threads counted, %d run elsewhere, %d wrong results\n",
			s_plain8, retro_atomic_load_acquire_int(&s_elsewhere8), retro_atomic_load_acquire_int(&s_wrong8));
	}

	// The SPU limits as one word: the same answers as the scan of every
	// group they replace, for random sets of groups; and from four threads at
	// once, two groups that do not both fit are never both taken
	{
		struct group_model { u32 type; bool sched; u32 num; };

		// The old check: every group scanned
		const auto old_check = [](const std::vector<group_model>& groups, u32 raw, u32 max_raw, u32 max_spu, const limits_data& init)
		{
			u32 physical_spus_count = init.physical;
			u32 raw_spu_count = init.raw_spu;
			u32 controllable_spu_count = init.controllable;
			u32 system_coop = init.controllable != 0 && init.physical != 0 ? 1 : 0;
			const u32 spu_limit = init.spu_limit != umax ? init.spu_limit : max_spu;
			const u32 raw_limit = init.raw_limit != umax ? init.raw_limit : max_raw;
			for (const auto& g : groups)
			{
				if (g.type & SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM)
				{
					system_coop++;
					controllable_spu_count = std::max<u32>(controllable_spu_count, 1);
					physical_spus_count += g.num - 1;
				}
				else if (g.sched)
					controllable_spu_count = std::max<u32>(controllable_spu_count, g.num);
				else
					physical_spus_count += g.num;
			}
			raw_spu_count += raw;
			if (spu_limit + raw_limit > 6 || raw_spu_count > raw_limit || physical_spus_count >= spu_limit || controllable_spu_count > spu_limit)
				return false;
			return system_coop <= 1;
		};

		usz differ = 0, taken = 0;
		for (u32 round = 0; round < 2000; round++)
		{
			spu_limits_t limits;
			const u32 max_raw = rnd() % 3;
			const u32 max_spu = 6 - max_raw - rnd() % 2;
			retro_atomic_store_release_64(&limits.word, static_cast<s64>(spu_limits_t::put(max_raw, 8) | spu_limits_t::put(max_spu, 11)));
			std::vector<group_model> groups;
			u32 raw = 0;

			for (u32 step = 0; step < 40; step++)
			{
				const u32 kind = rnd() % 4;
				if (kind == 3 && !groups.empty())
				{
					// Destroyed
					const usz i = rnd() % groups.size();
					limits.give_back(spu_limits_t::group_part(groups[i].type, groups[i].sched, groups[i].num));
					groups.erase(groups.begin() + i);
					continue;
				}

				group_model g{};
				limits_data init{};
				g.num = 1 + rnd() % 6;
				if (kind == 0)
				{
					g.type = SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM;
					init.controllable = 1;
					init.physical = g.num - 1;
				}
				else if (kind == 1)
				{
					g.sched = true;
					init.controllable = g.num;
				}
				else
				{
					init.physical = g.num;
				}

				const bool want = old_check(groups, raw, max_raw, max_spu, init);
				const bool got = limits.take(init, spu_limits_t::group_part(g.type, g.sched, g.num));
				differ += want != got;
				if (got)
				{
					groups.push_back(g);
					taken++;
				}

				// A raw SPU now and then
				if (rnd() % 4 == 0)
				{
					const bool want_raw = old_check(groups, raw, max_raw, max_spu, limits_data{.raw_spu = 1});
					const bool got_raw = limits.take(limits_data{.raw_spu = 1}, spu_limits_t::put(1, 5));
					differ += want_raw != got_raw;
					raw += got_raw;
				}
			}
		}
		CHECK(differ == 0);
		CHECK(taken > 1000);

		static spu_limits_t s_limits;
		static retro_atomic_int_t s_holding, s_bad9, s_stop9, s_got9;
		retro_atomic_store_release_64(&s_limits.word, static_cast<s64>(spu_limits_t::put(6, 11)));

		std::thread groupers[4];
		for (auto& g : groupers)
		{
			g = std::thread([]
			{
				// Groups of 3 physical SPUs: one fits, two do not (3 + 3 >= 6)
				const u64 part = spu_limits_t::group_part(0, false, 3);
				for (int n = 0; n < 100000; n++)
				{
					if (!s_limits.take(limits_data{.physical = 3}, part))
						continue;
					if (retro_atomic_fetch_add_int(&s_holding, 1) != 0)
						retro_atomic_fetch_add_int(&s_bad9, 1);
					retro_atomic_fetch_add_int(&s_got9, 1);
					retro_atomic_fetch_sub_int(&s_holding, 1);
					s_limits.give_back(part);
				}
			});
		}
		for (auto& g : groupers)
			g.join();

		CHECK(retro_atomic_load_acquire_int(&s_bad9) == 0);
		CHECK(retro_atomic_load_acquire_int(&s_got9) > 1000);
		CHECK(retro_atomic_load_acquire_64(&s_limits.word) == static_cast<s64>(spu_limits_t::put(6, 11)));
		std::printf("test_held_queues: SPU limits: %zu answers differ from the scan's (%zu groups taken); %d taken from 4 threads, %d when another held it\n",
			differ, taken, retro_atomic_load_acquire_int(&s_got9), retro_atomic_load_acquire_int(&s_bad9));
	}

	// cellVoice's queues to notify, in a savestate
	{
		typedef std::unordered_map<u64, std::deque<u64>> as_map;

		// One key: the map's bytes exactly
		{
			voice_manager manager;
			as_map map;
			for (u64 n = 0; n < 9; n++)
			{
				const voice_notify notify{0x1122334455667788ull, n * 1000003 + 5};
				CHECK(voice_notifies_push_back_growing(&manager.queue_keys, &notify));
				map[notify.key].push_back(notify.source);
			}
			utils::serial saved, expected;
			manager.save_queue_keys(saved);
			expected(map);
			CHECK(saved.data == expected.data);

			// None: one byte
			voice_manager none;
			utils::serial saved_none, expected_none;
			none.save_queue_keys(saved_none);
			expected_none(as_map{});
			CHECK(saved_none.data == expected_none.data);
		}

		// Several keys, set in turns: the map's own code reads what was
		// written here, and what the map wrote is read here
		{
			voice_manager manager;
			as_map map;
			for (u64 n = 0; n < 200; n++)
			{
				const voice_notify notify{0x5000 + rnd() % 7, u64{rnd()} << 16 | n};
				CHECK(voice_notifies_push_back_growing(&manager.queue_keys, &notify));
				map[notify.key].push_back(notify.source);
			}

			utils::serial saved;
			manager.save_queue_keys(saved);
			utils::serial in;
			in.set_reading_state(std::move(saved.data));
			as_map read;
			in(read);
			CHECK(read == map);

			utils::serial from_map;
			from_map(map);
			utils::serial in_map;
			in_map.set_reading_state(std::move(from_map.data));
			voice_manager back;
			back.load_queue_keys(in_map);
			as_map again;
			for (usz i = 0; i < voice_notifies_count(&back.queue_keys); i++)
				again[voice_notifies_at(&back.queue_keys, i)->key].push_back(voice_notifies_at(&back.queue_keys, i)->source);
			CHECK(again == map);

			// A key's queue unset (cellVoiceRemoveNotifyEventQueue): its
			// sources go, the others stay in order
			const u64 key = 0x5003;
			const usz had = map.count(key) ? map[key].size() : 0;
			CHECK(manager.remove_queue_key(key) == had && had != 0);
			CHECK(manager.remove_queue_key(key) == 0);
			map.erase(key);
			as_map left;
			for (usz i = 0; i < voice_notifies_count(&manager.queue_keys); i++)
				left[voice_notifies_at(&manager.queue_keys, i)->key].push_back(voice_notifies_at(&manager.queue_keys, i)->source);
			CHECK(left == map);
		}
	}

	// The DNS hook's made-up replies (Emu/NP/np_dnshook.cpp), through its
	// own code: one list of replies where there was a map of queues
	{
		g_cfg.net.swap_list.from_string("*.example.org=10.1.2.3");
		np::dnshook hook;

		// A query for @host, as a game's resolver sends it
		const auto query = [](u16 id, std::string_view host)
		{
			std::vector<u8> q{static_cast<u8>(id >> 8), static_cast<u8>(id), 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
			usz start = 0;
			while (start <= host.size())
			{
				const usz dot = std::min(host.find('.', start), host.size());
				q.push_back(static_cast<u8>(dot - start));
				q.insert(q.end(), host.begin() + start, host.begin() + dot);
				start = dot + 1;
			}
			q.insert(q.end(), {0, 0, 1, 0, 1});
			return q;
		};

		// A reply read as recvfrom reads it
		const auto take = [&](u32 sock)
		{
			std::vector<u8> out(512);
			out.resize(hook.take_dns_packet(sock, out.data(), 512));
			return out;
		};

		CHECK(!hook.is_dns(5));
		hook.add_dns_spy(5);
		hook.add_dns_spy(9);
		hook.add_dns_spy(5); // twice: still one
		CHECK(hook.is_dns(5) && hook.is_dns(9) && !hook.is_dns(6));
		CHECK(!hook.is_dns_queue(5) && !hook.is_dns_queue(9));

		// Not one of ours: no reply made
		const auto other = query(1, "www.elsewhere.net");
		CHECK(hook.analyze_dns_packet(5, other.data(), ::size32(other)) == -1);
		CHECK(!hook.is_dns_queue(5));

		// Replies on two sockets, in turns: each socket reads its own, in order
		for (u16 id = 100; id < 140; id++)
		{
			const auto q = query(id, "a.example.org");
			CHECK(hook.analyze_dns_packet(id & 1 ? 5 : 9, q.data(), ::size32(q)) == static_cast<s32>(q.size()));
		}
		CHECK(hook.is_dns_queue(5) && hook.is_dns_queue(9));
		usz wrong = 0;
		for (u16 id = 101; id < 121; id += 2)
		{
			const std::vector<u8> reply = take(5);
			wrong += reply.size() < 16 || reply[0] != (id >> 8) || reply[1] != (id & 0xff);
			wrong += reply.size() < 4 || std::memcmp(reply.data() + reply.size() - 4, "\x0a\x01\x02\x03", 4) != 0;
		}
		for (u16 id = 100; id < 140; id += 2)
		{
			const std::vector<u8> reply = take(9);
			wrong += reply.size() < 16 || reply[0] != (id >> 8) || reply[1] != (id & 0xff);
		}
		CHECK(wrong == 0);
		CHECK(hook.is_dns_queue(5) && !hook.is_dns_queue(9));

		// A reply is the query with the reply flags, one answer counted,
		// and the answer after it: byte for byte what the vector held
		{
			const auto q = query(0x1234, "x.example.org");
			CHECK(hook.analyze_dns_packet(9, q.data(), ::size32(q)) > 0);
			std::vector<u8> expected = q;
			expected[2] |= 0x80; // response
			expected[3] |= 0x80; // recursion available
			expected[7] = 1;     // one answer
			expected.insert(expected.end(), {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 0x3B, 0, 4, 10, 1, 2, 3});
			CHECK(take(9) == expected);
		}

		// A socket no longer watched: its unread replies go with it, the
		// other's stay
		{
			const auto q = query(500, "b.example.org");
			CHECK(hook.analyze_dns_packet(9, q.data(), ::size32(q)) > 0);
		}
		hook.remove_dns_spy(5);
		CHECK(!hook.is_dns(5) && !hook.is_dns_queue(5) && hook.is_dns(9) && hook.is_dns_queue(9));
		const std::vector<u8> last = take(9);
		CHECK(last.size() > 2 && last[0] == (500 >> 8) && last[1] == (500 & 0xff));

		// Answered without having been watched: watched from then on, as the map made it
		{
			const auto q = query(7, "c.example.org");
			CHECK(!hook.is_dns(33));
			CHECK(hook.analyze_dns_packet(33, q.data(), ::size32(q)) > 0);
			CHECK(hook.is_dns(33) && hook.is_dns_queue(33));
		}
		// (left unread: the hook's destructor frees it)
	}

	// The overlay's messages (Emu/RSX/Overlays/overlay_message.cpp),
	// through its own code. Each message holds the counter it was queued
	// with, so the counter's use count says whether the message is alive.
	{
		// No font to find here: the text is measured with none
		g_emu_callbacks.get_font_dirs = []() -> std::vector<std::string> { return {}; };

		typedef std::shared_ptr<retro_atomic_int_t> counter;
		std::vector<counter> refs;
		for (u32 i = 0; i < 6; i++)
			refs.emplace_back(std::make_shared<retro_atomic_int_t>(1));

		{
			rsx::overlays::message overlay;

			for (u32 i = 0; i < 5; i++)
				overlay.queue_message(fmt::format("message %u", i), umax, refs[i]);
			for (u32 i = 0; i < 5; i++)
				CHECK(refs[i].use_count() == 2);

			// The same text again: not queued twice
			overlay.queue_message(std::string("message 2"), umax, refs[5]);
			CHECK(refs[5].use_count() == 1);

			// Three go on screen, in the order queued; the other two wait
			overlay.update(0);
			for (u32 i = 0; i < 5; i++)
				CHECK(refs[i].use_count() == 2);

			// One on screen is done with (its counter at 0): it goes, the
			// rest stay, and its text can be queued again
			retro_atomic_store_release_int(refs[1].get(), 0);
			overlay.update(0);
			CHECK(refs[1].use_count() == 1);
			CHECK(refs[0].use_count() == 2 && refs[2].use_count() == 2 && refs[3].use_count() == 2 && refs[4].use_count() == 2);
			retro_atomic_store_release_int(refs[5].get(), 1);
			overlay.queue_message(std::string("message 1"), umax, refs[5]);
			CHECK(refs[5].use_count() == 2);

			// All done with: each update lets the visible ones go and
			// brings the waiting ones on, until none is left
			for (auto& ref : refs)
				retro_atomic_store_release_int(ref.get(), 0);
			for (u32 i = 0; i < 4; i++)
				overlay.update(0);
			usz alive = 0;
			for (auto& ref : refs)
				alive += ref.use_count() != 1;
			CHECK(alive == 0);

			// Left queued and on screen when the overlay goes: freed with it
			for (u32 i = 0; i < 5; i++)
			{
				retro_atomic_store_release_int(refs[i].get(), 1);
				overlay.queue_message(fmt::format("last %u", i), umax, refs[i]);
			}
			overlay.update(0);
			for (u32 i = 0; i < 5; i++)
				CHECK(refs[i].use_count() == 2);
		}

		usz alive = 0;
		for (auto& ref : refs)
			alive += ref.use_count() != 1;
		CHECK(alive == 0);
	}

	std::printf("test_held_queues: %s\n", failures ? "FAIL" : "ok");
	std::fflush(stdout);
	std::_Exit(failures ? 1 : 0);
}
