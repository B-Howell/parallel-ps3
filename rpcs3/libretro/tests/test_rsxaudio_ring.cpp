// sys_rsxaudio's audio on its way to the backend (Emu/Cell/lv2/
// sys_rsxaudio.cpp, the path a game takes with its own libaudio): the
// lock-free ring cellAudio uses (libretro_audio_ring) where there was a ring
// that copied under a 16-byte compare-and-swap, and the backend's callback
// converting straight out of the ring when it has nothing else to do to the
// samples.
//
// - what the callback hands the backend is what it handed it before: the
//   samples in order, soft-clipped, for float and for 16-bit output, with
//   the volume applied when it is not at rest (the copying path) - checked
//   against the same conversion done on the samples that went in;
// - round the ring's wrap many times, nothing lost or repeated;
// - asked for more than is waiting, it gives what there is;
// - a flush from the writer's side (the backend starting) drops what was
//   waiting and nothing after it;
// - the backend thread takes no lock and waits for nobody: what it wants
//   of the rings (a flush, their limits, the dump's ring) is a request the
//   data thread carries out before it next writes, and goes on writing
//   after; the stream's new ports and the other news reach it as bits of
//   one word and through a sequence count;
// - the hardware's parameters change on the HLE thread, one change at a
//   time, and reach the data thread through a sequence count: three
//   threads changing them at once lose no change, and the data thread
//   never sees half of one;
// - the game's calls on the rsxaudio object run on the data thread, which
//   takes them up at once, even while it waits with no timer running.
// - sys_uart's two rings (retro_spsc) against the ring they were, the same
//   bytes and counts through 200000 writes and reads;
// - with RSXAUDIO_BENCH=1 it is timed: a callback's worth in
//   and out, against the ring it replaced.
//
// Built by `make check` from the core's own objects and archives, the same
// link as the core's with this main.

#include "stdafx.h"
#include "Emu/Cell/lv2/sys_rsxaudio.h"
#include "Emu/Audio/AudioBackend.h"
#include "Emu/Cell/lv2/sys_uart.h"
#include "Emu/IdManager.h"
#include "reference_ringbuf.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// The n-th sample of the stream: mostly in range, now and then loud enough to be clipped
static f32 sample_of(u64 n)
{
	const f32 v = static_cast<f32>(static_cast<s32>((n * 2654435761u) & 0xffff) - 32768) / 32768.0f;
	return n % 37 == 0 ? v * 1.6f : v * 0.8f;
}

struct rsxaudio_backend_test
{
	typedef rsxaudio_backend_thread::callback_config config;

	static void setup(rsxaudio_backend_thread& thread, usz ring_bytes)
	{
		lr_audio_ring_free(&thread.ringbuf);
		CHECK(lr_audio_ring_init(&thread.ringbuf, ring_bytes));
		thread.callback_tmp_buf.resize(8192);
	}

	static lr_audio_ring& ring(rsxaudio_backend_thread& thread) { return thread.ringbuf; }

	static u32 fill(rsxaudio_backend_thread& thread, const config& cfg, u32 bytes, void* out)
	{
		return thread.fill_from_ring(cfg, bytes, out);
	}

	static void request(rsxaudio_backend_thread& thread, bool aux, usz limit, usz aux_limit) { thread.request_rings(aux, limit, aux_limit); }
	static void apply(rsxaudio_backend_thread& thread) { thread.apply_ring_request(); }
	static bool applied(rsxaudio_backend_thread& thread) { return retro_atomic_load_acquire_int(&thread.ring_applied) == retro_atomic_load_acquire_int(&thread.ring_request); }
	static bool data_aux(rsxaudio_backend_thread& thread) { return thread.data_aux_ringbuf; }
	static lr_audio_ring& aux_ring(rsxaudio_backend_thread& thread) { return thread.aux_ringbuf; }
	static void ports(rsxaudio_backend_thread& thread, AudioFreq freq)
	{
		std::array<rsxaudio_backend_thread::port_config, SYS_RSXAUDIO_AVPORT_CNT> cfg{};
		for (auto& port : cfg)
			port.freq = freq;
		thread.set_new_stream_param(cfg, {});
	}
	static int take_events(rsxaudio_backend_thread& thread) { return retro_atomic_exchange_int(&thread.events, 0); }
	static AudioFreq posted_freq(rsxaudio_backend_thread& thread)
	{
		for (;;)
		{
			const int seq = retro_seqlock_read_begin(&thread.ra_state_seq);
			const AudioFreq freq = thread.new_ra_state.port[0].freq;
			if (!retro_seqlock_read_retry(&thread.ra_state_seq, seq))
				return freq;
		}
	}
	static bool cfg_changed(rsxaudio_backend_thread& thread) { return thread.callback_cfg_now().cfg_changed; }
	static rsxaudio_hw_param_t hw_param(rsxaudio_data_thread& thread) { return thread.hw_param_now(); }

	static config stereo(bool s16, u16 volume)
	{
		config cfg{};
		cfg.ready = true;
		cfg.input_ch_cnt = 2;
		cfg.output_channel_layout = static_cast<u8>(audio_channel_layout::stereo);
		cfg.convert_to_s16 = s16;
		cfg.target_volume = volume;
		cfg.current_volume = volume;
		cfg.initial_volume = volume;
		return cfg;
	}
};

typedef rsxaudio_backend_test T;

// Pushes samples @from.. as the data thread does, in blocks; the next sample's number
static u64 push(lr_audio_ring& ring, u64 from, u32 samples)
{
	std::vector<f32> block(samples);
	for (u32 i = 0; i < samples; i++)
		block[i] = sample_of(from + i);
	CHECK(lr_audio_ring_push(&ring, block.data(), samples * sizeof(f32), false) == samples * sizeof(f32));
	return from + samples;
}

static double now_ns()
{
	return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main()
{
	rsxaudio_backend_thread thread;
	T::setup(thread, 4096 * sizeof(f32));
	lr_audio_ring& ring = T::ring(thread);

	std::vector<f32> out(4096), expected(4096), work(4096);
	std::vector<s16> out16(4096), expected16(4096);

	// Float out, volume at rest: converted straight out of the ring. Blocks
	// of sizes that do not divide the ring, round it many times
	{
		const T::config cfg = T::stereo(false, T::config::VOL_NOMINAL);
		u64 in = 0, taken = 0, wrong = 0;

		for (u32 step = 0; step < 20000; step++)
		{
			const u32 samples = 2 * (100 + step % 411);
			in = push(ring, in, samples);

			const u32 got = T::fill(thread, cfg, samples * sizeof(f32), out.data());
			CHECK(got == samples * sizeof(f32));

			for (u32 i = 0; i < samples; i++)
				work[i] = sample_of(taken + i);
			AudioBackend::normalize(samples, work.data(), expected.data());
			wrong += std::memcmp(out.data(), expected.data(), samples * sizeof(f32)) != 0;
			taken += samples;
		}
		CHECK(wrong == 0 && taken == in);
		CHECK(lr_audio_ring_used(&ring) == 0);
	}

	// 16-bit out, the same way
	{
		const T::config cfg = T::stereo(true, T::config::VOL_NOMINAL);
		u64 in = 1000, taken = 1000, wrong = 0;

		for (u32 step = 0; step < 5000; step++)
		{
			const u32 samples = 2 * (64 + step % 300);
			in = push(ring, in, samples);

			const u32 got = T::fill(thread, cfg, samples * sizeof(s16), out16.data());
			CHECK(got == samples * sizeof(s16));

			for (u32 i = 0; i < samples; i++)
				work[i] = sample_of(taken + i);
			AudioBackend::convert_to_s16(samples, work.data(), expected16.data());
			wrong += std::memcmp(out16.data(), expected16.data(), samples * sizeof(s16)) != 0;
			taken += samples;
		}
		CHECK(wrong == 0);
	}

	// Volume not at rest: the samples are copied out, scaled, then converted
	{
		const T::config cfg = T::stereo(false, 5000);
		u64 in = 7, taken = 7, wrong = 0;

		for (u32 step = 0; step < 5000; step++)
		{
			const u32 samples = 2 * (64 + step % 300);
			in = push(ring, in, samples);

			const u32 got = T::fill(thread, cfg, samples * sizeof(f32), out.data());
			CHECK(got == samples * sizeof(f32));

			for (u32 i = 0; i < samples; i++)
				work[i] = sample_of(taken + i);
			AudioBackend::apply_volume_static(0.5f, samples, work.data(), work.data());
			AudioBackend::normalize(samples, work.data(), expected.data());
			wrong += std::memcmp(out.data(), expected.data(), samples * sizeof(f32)) != 0;
			taken += samples;
		}
		CHECK(wrong == 0);
	}

	// Asked for more than is waiting: what there is, in order, and then nothing
	{
		const T::config cfg = T::stereo(false, T::config::VOL_NOMINAL);
		push(ring, 50000, 200);
		const u32 got = T::fill(thread, cfg, 512 * sizeof(f32), out.data());
		CHECK(got == 200 * sizeof(f32));
		for (u32 i = 0; i < 200; i++)
			work[i] = sample_of(50000 + i);
		AudioBackend::normalize(200, work.data(), expected.data());
		CHECK(std::memcmp(out.data(), expected.data(), 200 * sizeof(f32)) == 0);

		// Empty: silence for the whole request
		std::fill(out.begin(), out.end(), 1.0f);
		CHECK(T::fill(thread, cfg, 512 * sizeof(f32), out.data()) == 512 * sizeof(f32));
		usz loud = 0;
		for (u32 i = 0; i < 512; i++)
			loud += out[i] != 0.0f;
		CHECK(loud == 0);
	}

	// The backend starting: what was waiting is dropped, what comes after is not
	{
		const T::config cfg = T::stereo(false, T::config::VOL_NOMINAL);
		push(ring, 111, 600);
		lr_audio_ring_flush(&ring);
		CHECK(lr_audio_ring_used(&ring) == 0);
		push(ring, 90000, 300);
		const u32 got = T::fill(thread, cfg, 300 * sizeof(f32), out.data());
		CHECK(got == 300 * sizeof(f32));
		for (u32 i = 0; i < 300; i++)
			work[i] = sample_of(90000 + i);
		AudioBackend::normalize(300, work.data(), expected.data());
		CHECK(std::memcmp(out.data(), expected.data(), 300 * sizeof(f32)) == 0);
	}

	// sys_uart's rings (retro_spsc behind uart_ring_push and uart_ring_pop)
	// against the ring they were: a write takes as many bytes as fit, a
	// read as many as there are, the same bytes either way
	{
		retro_spsc_t q{};
		CHECK(retro_spsc_init(&q, PS3AV_TX_BUF_SIZE));
		reference_ringbuf old_ring(PS3AV_TX_BUF_SIZE);
		u8 in[3000], a[3000], b[3000];
		u32 seed = 12345;
		usz wrong = 0;

		for (u32 step = 0; step < 200000; step++)
		{
			seed = seed * 1664525 + 1013904223;
			const u32 n = (seed >> 8) % 3000;

			if (seed & 1)
			{
				for (u32 i = 0; i < n; i++)
					in[i] = static_cast<u8>(seed >> (i % 24)) ^ static_cast<u8>(i);
				wrong += uart_ring_push(q, PS3AV_TX_BUF_SIZE, in, n) != old_ring.push(in, n, true);
			}
			else
			{
				const u32 got = uart_ring_pop(q, a, n);
				wrong += got != old_ring.pop(b, n, true);
				wrong += std::memcmp(a, b, got) != 0;
			}
			wrong += uart_ring_used(q) != old_ring.get_used_size();
		}
		CHECK(wrong == 0);
		retro_spsc_free(&q);
	}

	// A callback's worth (256 stereo frames) in and out, against the ring replaced
	if (std::getenv("RSXAUDIO_BENCH"))
	{
		const T::config cfg = T::stereo(false, T::config::VOL_NOMINAL);
		const u32 samples = 512, rounds = 2000000;
		std::vector<f32> block(samples);
		for (u32 i = 0; i < samples; i++)
			block[i] = sample_of(i) * 0.5f;

		double best_old = 1e18, best_new = 1e18;
		for (u32 k = 0; k < 5; k++)
		{
			reference_ringbuf old_ring(4096 * sizeof(f32));
			double t = now_ns();
			for (u32 i = 0; i < rounds; i++)
			{
				old_ring.push(block.data(), samples * sizeof(f32));
				old_ring.pop(work.data(), samples * sizeof(f32), true);
				AudioBackend::normalize(samples, work.data(), out.data());
			}
			best_old = std::min(best_old, (now_ns() - t) / rounds);

			t = now_ns();
			for (u32 i = 0; i < rounds; i++)
			{
				lr_audio_ring_push(&ring, block.data(), samples * sizeof(f32), false);
				T::fill(thread, cfg, samples * sizeof(f32), out.data());
			}
			best_new = std::min(best_new, (now_ns() - t) / rounds);
		}
		std::printf("test_rsxaudio_ring: 256 stereo frames in and out: %.0f ns with the old ring, %.0f ns now\n", best_old, best_new);

		// A 64-byte packet through a UART ring
		retro_spsc_t q{};
		CHECK(retro_spsc_init(&q, PS3AV_TX_BUF_SIZE));
		reference_ringbuf old_uart(PS3AV_TX_BUF_SIZE);
		u8 packet[64]{}, back[64];
		u64 sink = 0;
		best_old = best_new = 1e18;
		for (u32 k = 0; k < 5; k++)
		{
			double t = now_ns();
			for (u32 i = 0; i < 5000000; i++)
			{
				packet[0] = static_cast<u8>(i);
				old_uart.push(packet, 64, true);
				sink += old_uart.pop(back, 64, true) + back[0];
			}
			best_old = std::min(best_old, (now_ns() - t) / 5000000);

			t = now_ns();
			for (u32 i = 0; i < 5000000; i++)
			{
				packet[0] = static_cast<u8>(i);
				uart_ring_push(q, PS3AV_TX_BUF_SIZE, packet, 64);
				sink += uart_ring_pop(q, back, 64) + back[0];
			}
			best_new = std::min(best_new, (now_ns() - t) / 5000000);
		}
		std::printf("test_rsxaudio_ring: a 64-byte UART packet in and out: %.1f ns with the old ring, %.1f ns now (%llu)\n", best_old, best_new, static_cast<unsigned long long>(sink & 1));
		retro_spsc_free(&q);
	}

	// The backend thread's requests, carried out by the data thread
	{
		rsxaudio_backend_thread t;
		lr_audio_ring& main = T::ring(t);
		const T::config cfg = T::stereo(false, T::config::VOL_NOMINAL);
		T::request(t, false, 4096 * sizeof(f32), 0);
		CHECK(!T::applied(t));
		T::apply(t);
		CHECK(T::applied(t) && !T::data_aux(t) && main.limit == 4096 * sizeof(f32));

		// Written before a flush asked for: not heard after it, and the
		// data thread writes on
		u64 in = push(main, 0, 2000);
		T::request(t, false, 4096 * sizeof(f32), 0); // the backend starting
		CHECK(lr_audio_ring_used(&main) == 2000 * sizeof(f32)); // nothing done by the asking
		T::apply(t);
		CHECK(lr_audio_ring_used(&main) == 0);
		const u64 after = in;
		in = push(main, in, 1000);
		const u32 got = T::fill(t, cfg, 1000 * sizeof(f32), out.data());
		CHECK(got == 1000 * sizeof(f32));
		for (u32 i = 0; i < 1000; i++)
			work[i] = sample_of(after + i);
		AudioBackend::normalize(1000, work.data(), expected.data());
		CHECK(std::memcmp(out.data(), expected.data(), 1000 * sizeof(f32)) == 0);

		// The dump: the data thread goes over to the aux ring, made then
		T::request(t, true, 2048 * sizeof(f32), 2048 * sizeof(f32));
		T::apply(t);
		CHECK(T::data_aux(t) && T::aux_ring(t).q.buffer && T::aux_ring(t).limit == 2048 * sizeof(f32) && main.limit == 2048 * sizeof(f32));

		// The new ports: published through the sequence count, the bit set,
		// and the callback told to wait for the backend to be set up again
		while (T::take_events(t)) {}
		T::ports(t, AudioFreq::FREQ_96K);
		CHECK(T::posted_freq(t) == AudioFreq::FREQ_96K);
		CHECK(T::take_events(t) & 2);
		CHECK(T::cfg_changed(t));
		std::printf("test_rsxaudio_ring: the backend thread's requests: a flush asked for and carried out by the writer, the dump's ring taken up, the ports posted\n");
	}

	// The hardware's parameters: three threads change them, one reads
	{
		g_fxo->reset();
		g_fxo->init<rsx_audio_backend>();
		// Large (its output buffer): on the heap, not the stack
		const auto data_ptr = std::make_unique<rsxaudio_data_thread>();
		rsxaudio_data_thread& data = *data_ptr;
		constexpr int n = 3000;
		std::atomic<bool> go{false}, stop{false};
		std::atomic<u32> torn{0}, reads{0};
		// Each change counts one on: the count in bytes far apart in the
		// parameters, all alike
		const auto changer = [&]()
		{
			return std::thread([&]
			{
				while (!go) {}
				for (int i = 0; i < n; i++)
				{
					data.update_hw_param([](rsxaudio_hw_param_t& p)
					{
						const u8 next = static_cast<u8>(p.spdif[0].cs_data[0] + 1);
						// Slow to change it (no matter on the HLE thread, where
						// changes are one at a time)
						std::this_thread::yield();
						p.spdif[0].cs_data.fill(next);
						p.hdmi[1].info_frame.fill(next);
					});
				}
			});
		};
		// From a count of 0, all alike
		data.update_hw_param([](rsxaudio_hw_param_t& p)
		{
			p.spdif[0].cs_data.fill(0);
			p.hdmi[1].info_frame.fill(0);
		});
		std::thread a = changer(), b = changer();
		std::thread c([&]
		{
			while (!go) {}
			for (int i = 0; i < n; i++)
				data.update_mute_state(RsxaudioPort::SERIAL, i & 1);
		});
		std::thread reader([&]
		{
			while (!go) {}
			while (!stop)
			{
				const rsxaudio_hw_param_t p = T::hw_param(data);
				const u8 count = p.spdif[0].cs_data[0];
				for (u8 v : p.spdif[0].cs_data)
					torn += v != count;
				for (u8 v : p.hdmi[1].info_frame)
					torn += v != count;
				reads++;
			}
		});
		go = true;
		a.join();
		b.join();
		c.join();
		stop = true;
		reader.join();
		const rsxaudio_hw_param_t last = T::hw_param(data);
		CHECK(last.spdif[0].cs_data[0] == static_cast<u8>(2 * n) && last.hdmi[1].info_frame[0] == static_cast<u8>(2 * n));
		CHECK(last.serial.muted == ((n - 1) & 1));
		CHECK(torn == 0);
		std::printf("test_rsxaudio_ring: the hardware's parameters: %d changes from each of three threads, the count at %u (%u expected), %u torn in %u reads\n",
			n, last.spdif[0].cs_data[0], static_cast<u8>(2 * n), torn.load(), reads.load());
	}

	// The game's calls on the object: run by the data thread, at once
	{
		g_fxo->init<rsx_audio_data>();
		auto& data = g_fxo->get<rsx_audio_data>();
		// Let it get to its wait, no timer running
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		u32 on_data = 0, quick = 0;
		double worst_ms = 0;
		for (int i = 0; i < 20; i++)
		{
			std::atomic<bool> done{false};
			std::thread::id where{};
			const auto t0 = std::chrono::steady_clock::now();
			std::thread caller([&]
			{
				owner_calls_on(&data.calls, false, [&]() { where = std::this_thread::get_id(); });
				done = true;
			});
			while (!done && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
				std::this_thread::yield();
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			worst_ms = std::max(worst_ms, ms);
			if (!done)
			{
				// Never taken up: let the thread go so the test ends
				data = thread_state::aborting;
			}
			caller.join();
			quick += done && ms < 100.0;
			on_data += done && where != std::thread::id{} && where != caller.get_id() && where != std::this_thread::get_id();
			if (!done)
				break;
		}
		CHECK(quick == 20);
		CHECK(on_data == 20);
		std::printf("test_rsxaudio_ring: calls on the data thread: %u of 20 taken up at once (worst %.2f ms), %u run there\n", quick, worst_ms, on_data);
	}

	std::printf("test_rsxaudio_ring: %s\n", failures ? "FAIL" : "ok");
	std::fflush(stdout);
	std::_Exit(failures ? 1 : 0);
}
