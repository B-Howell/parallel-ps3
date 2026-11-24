// The game's audio on its way to the frontend (libretro_audio.cpp): each
// retro_run pulls what is due from cellAudio's ring, the write callback,
// and hands it over.
//
// - a frontend that takes floats (multi-channel, batch_float) gets the
//   game's floats as they are, bit for bit, in order across runs;
// - one that takes s16 gets them converted, in order, stereo through the
//   plain batch callback;
// - cellAudio's ring is lent, not copied: the frontend is handed pointers
//   into the ring's own memory, the samples in order across the wrap and
//   across a flush (nothing written before it is heard after it), and
//   silence when the ring has less than is due;
// - while cellAudio's thread opens, closes and sets the callback over and
//   over, a run never waits for it: no run takes a millisecond;
// - nor does cellAudio's thread wait for a run: opening, closing and
//   ending the lend while retro_run has samples of the ring on loan;
// - an audio output's settings (cellAudioOut), one word without a lock:
//   three threads changing parts of them at once lose none of the
//   changes, and a reader never sees half of one;
// - cellMusicDecode's decoder hands its tracks over without a lock: a
//   reader taking them as they are decoded gets every sample of both
//   tracks of a playlist, in order, the second once it is done with the
//   first.
//
// Built by `make check` from the core's own objects and archives, the same
// link as the core's with this main.

#include "stdafx.h"
#include "libretro_audio.h"
#include "Emu/Cell/Modules/cellAudioOut.h"
#include "util/media_utils.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// The game's samples: a value per sample that says where it is in the stream
static u32 s_produced = 0;
static float sample_value(u32 n) { return static_cast<float>(static_cast<int>(n % 20000) - 10000) / 16384.0f; }

static u32 produce(u32 bytes, void* out)
{
	float* f = static_cast<float*>(out);
	for (u32 i = 0; i < bytes / sizeof(float); i++)
		f[i] = sample_value(s_produced++);
	return bytes;
}

// As a backend takes it: a plain function and its object (none here)
static const audio_write_cb produce_cb{[](void*, u32 bytes, void* out) { return produce(bytes, out); }, nullptr};

static std::vector<float> s_got_f32;
static std::vector<s16> s_got_s16;
static usz s_got_channels = 0;
// Where the frontend's floats came from: inside this memory, or not
static const u8* s_lend_begin = nullptr;
static const u8* s_lend_end = nullptr;
static usz s_from_ring = 0, s_not_from_ring = 0;
static usz s_batch_calls = 0;

static size_t RETRO_CALLCONV take_f32(const float* data, size_t frames, unsigned channels, unsigned)
{
	s_got_channels = channels;
	const u8* p = reinterpret_cast<const u8*>(data);
	if (p >= s_lend_begin && p + frames * channels * sizeof(float) <= s_lend_end)
		s_from_ring++;
	else
		s_not_from_ring++;
	s_got_f32.insert(s_got_f32.end(), data, data + frames * channels);
	return frames;
}

static size_t RETRO_CALLCONV take_multi_s16(const int16_t* data, size_t frames, unsigned channels, unsigned)
{
	s_got_channels = channels;
	s_got_s16.insert(s_got_s16.end(), data, data + frames * channels);
	return frames;
}

static size_t RETRO_CALLCONV take_s16(const int16_t* data, size_t frames)
{
	s_batch_calls++;
	s_got_s16.insert(s_got_s16.end(), data, data + frames * 2);
	return frames;
}

static bool s_offer_float = true;

static bool RETRO_CALLCONV environ_multi(unsigned cmd, void* data)
{
	if (cmd != RETRO_ENVIRONMENT_GET_AUDIO_SAMPLE_BATCH_MULTI)
		return false;
	auto* cb = static_cast<retro_audio_sample_multi_callback*>(data);
	cb->batch_int16 = take_multi_s16;
	cb->batch_float = s_offer_float ? take_f32 : nullptr;
	return true;
}

static bool RETRO_CALLCONV environ_none(unsigned, void*) { return false; }

// Runs back to back, as fast as they come: the audio is paced by the runs,
// not by the time between them
static void run_for(int runs, double fps = 60.0)
{
	for (int i = 0; i < runs; i++)
		libretro_audio_process(take_s16, fps);
}

// A member function as the backend takes it (audio_write_cb::of,
// audio_state_cb::of): the member called on that object, with what it was
// given, nothing allocated; it was a std::function around std::bind
struct bound_writer
{
	u32 bytes = 0;
	void* buf = nullptr;
	AudioStateEvent event{};

	u32 write(u32 b, void* p) { bytes = b; buf = p; return b / 2; }
	void state(AudioStateEvent e) { event = e; }
};

int main()
{
	{
		bound_writer w;
		int dummy = 0;
		const audio_write_cb wcb = audio_write_cb::of<&bound_writer::write>(&w);
		const audio_state_cb scb = audio_state_cb::of<&bound_writer::state>(&w);
		CHECK(static_cast<bool>(wcb) && static_cast<bool>(scb) && !audio_write_cb{} && !audio_state_cb{nullptr});
		CHECK(wcb(96, &dummy) == 48 && w.bytes == 96 && w.buf == &dummy);
		scb(AudioStateEvent::DEFAULT_DEVICE_MAYBE_CHANGED);
		CHECK(w.event == AudioStateEvent::DEFAULT_DEVICE_MAYBE_CHANGED);
		std::printf("test_audio_frontend: a member function bound as a plain write and state callback; ok\n");
	}

	// Floats to a frontend that takes them
	{
		CHECK(libretro_audio_negotiate_multi(environ_multi));
		LibretroAudioBackend backend;
		CHECK(backend.Open("", AudioFreq::FREQ_48K, AudioSampleSize::FLOAT, AudioChannelCnt::STEREO, audio_channel_layout::automatic));
		backend.SetWriteCallback(produce_cb);
		backend.Play();
		s_produced = 0;
		run_for(10);
		CHECK(s_got_f32.size() == 48000 / 60 * 2 * 10); // exactly a frame's worth a run
		CHECK(s_got_s16.empty());
		CHECK(s_got_channels == 2);
		usz wrong = 0;
		for (usz i = 0; i < s_got_f32.size(); i++)
			wrong += s_got_f32[i] != sample_value(static_cast<u32>(i));
		CHECK(wrong == 0);
		backend.Close();
	}

	// s16 to a frontend with multi-channel output but no floats
	{
		s_offer_float = false;
		s_got_f32.clear();
		s_got_s16.clear();
		CHECK(libretro_audio_negotiate_multi(environ_multi));
		LibretroAudioBackend backend;
		CHECK(backend.Open("", AudioFreq::FREQ_48K, AudioSampleSize::FLOAT, AudioChannelCnt::STEREO, audio_channel_layout::automatic));
		backend.SetWriteCallback(produce_cb);
		backend.Play();
		s_produced = 0;
		run_for(10);
		CHECK(s_got_f32.empty());
		CHECK(s_got_s16.size() == 48000 / 60 * 2 * 10);
		usz wrong = 0;
		for (usz i = 0; i < s_got_s16.size(); i++)
			wrong += std::abs(s_got_s16[i] - sample_value(static_cast<u32>(i)) * 32768.0f) > 1.0f;
		CHECK(wrong == 0);
		backend.Close();
	}

	// s16 stereo through the plain batch callback
	{
		s_got_s16.clear();
		CHECK(!libretro_audio_negotiate_multi(environ_none));
		LibretroAudioBackend backend;
		CHECK(backend.Open("", AudioFreq::FREQ_48K, AudioSampleSize::FLOAT, AudioChannelCnt::STEREO, audio_channel_layout::automatic));
		backend.SetWriteCallback(produce_cb);
		backend.Play();
		s_produced = 0;
		run_for(5);
		CHECK(s_batch_calls > 0);
		CHECK(s_got_s16.size() == 48000 / 60 * 2 * 5);
		backend.Close();
	}

	// NTSC: 48000 / 59.94 = 800.8 a run, the fractions carried, whatever the
	// time between runs; and silence, the same amount, while not playing
	{
		s_got_s16.clear();
		CHECK(!libretro_audio_negotiate_multi(environ_none));
		LibretroAudioBackend backend;
		CHECK(backend.Open("", AudioFreq::FREQ_48K, AudioSampleSize::FLOAT, AudioChannelCnt::STEREO, audio_channel_layout::automatic));
		backend.SetWriteCallback(produce_cb);
		backend.Play();
		const double ntsc = 60.0 * 1000.0 / 1001.0;
		// 5005 runs of 800.8: 4008004 frames, give or take the fraction
		// carried in, and every run 800 or 801
		u32 odd_runs = 0;
		s_got_s16.clear();
		for (int i = 0; i < 5005; i++)
		{
			const usz before = s_got_s16.size();
			libretro_audio_process(take_s16, ntsc);
			const usz got = (s_got_s16.size() - before) / 2;
			odd_runs += got != 800 && got != 801;
		}
		const usz total = s_got_s16.size() / 2;
		CHECK(total + 1 >= 4008004 && total <= 4008004 + 1);
		CHECK(odd_runs == 0);
		backend.Pause();
		s_got_s16.clear();
		run_for(60);
		CHECK(s_got_s16.size() == 48000u * 2);
		bool silent = true;
		for (s16 v : s_got_s16)
			silent = silent && v == 0;
		CHECK(silent);
		backend.Close();
	}

	// cellAudio's ring, lent: the frontend reads the ring's own memory
	{
		s_offer_float = true;
		s_got_f32.clear();
		s_got_s16.clear();
		CHECK(libretro_audio_negotiate_multi(environ_multi));
		LibretroAudioBackend backend;
		CHECK(backend.Open("", AudioFreq::FREQ_48K, AudioSampleSize::FLOAT, AudioChannelCnt::STEREO, audio_channel_layout::automatic));

		// The backend's own ring, holding a tenth of a second
		lr_audio_ring* lent = backend.LendRing(4800 * 2 * sizeof(float));
		CHECK(lent != nullptr);
		lr_audio_ring& ring = *lent;
		// The ring's memory, both mappings of it
		s_lend_begin = ring.q.buffer;
		s_lend_end = ring.q.buffer + ring.q.capacity * 2;
		backend.Play();

		u32 produced = 0;
		const auto fill = [&](u32 frames)
		{
			std::vector<float> block(frames * 2);
			for (float& f : block)
				f = sample_value(produced++);
			CHECK(lr_audio_ring_push(&ring, block.data(), block.size() * sizeof(float), false) == block.size() * sizeof(float));
		};

		// 200 runs, the ring filled before each: round its end many times
		s_from_ring = s_not_from_ring = 0;
		for (int i = 0; i < 200; i++)
		{
			fill(800);
			libretro_audio_process(take_s16, 60.0);
		}
		CHECK(s_got_f32.size() == 200u * 800 * 2);
		usz wrong = 0;
		for (usz i = 0; i < s_got_f32.size(); i++)
			wrong += s_got_f32[i] != sample_value(static_cast<u32>(i));
		CHECK(wrong == 0);
		CHECK(ring.q.mirror ? s_from_ring == 200 && s_not_from_ring == 0 : s_from_ring > 0);
		CHECK(lr_audio_ring_active(&ring));
		std::printf("test_audio_frontend: 200 runs from cellAudio's ring: %zu handed over from the ring's own memory, %zu from a copy\n", s_from_ring, s_not_from_ring);

		// Less in the ring than is due: what is there, then silence
		s_got_f32.clear();
		fill(300);
		libretro_audio_process(take_s16, 60.0);
		CHECK(s_got_f32.size() == 800u * 2);
		wrong = 0;
		for (usz i = 0; i < 600; i++)
			wrong += s_got_f32[i] != sample_value(200u * 1600 + static_cast<u32>(i));
		for (usz i = 600; i < 1600; i++)
			wrong += s_got_f32[i] != 0.0f;
		CHECK(wrong == 0);

		// A flush: what was written before it is not heard after it
		s_got_f32.clear();
		fill(800);
		lr_audio_ring_flush(&ring);
		CHECK(!lr_audio_ring_active(&ring));
		const u32 after_flush = produced;
		fill(800);
		libretro_audio_process(take_s16, 60.0);
		CHECK(s_got_f32.size() == 800u * 2);
		wrong = 0;
		for (usz i = 0; i < s_got_f32.size(); i++)
			wrong += s_got_f32[i] != sample_value(after_flush + static_cast<u32>(i));
		CHECK(wrong == 0);

		// The lend ended: silence, and the ring is not read again
		backend.EndLend();
		fill(800);
		const usz used = lr_audio_ring_used(&ring);
		s_got_f32.clear();
		s_got_s16.clear();
		libretro_audio_process(take_s16, 60.0);
		CHECK(lr_audio_ring_used(&ring) == used);
		CHECK(s_got_f32.empty() && s_got_s16.size() == 800u * 2);

		backend.Close();
		s_lend_begin = s_lend_end = nullptr;
	}

	// cellAudio's thread busy with the backend: no run waits
	{
		CHECK(libretro_audio_negotiate_multi(environ_multi));
		LibretroAudioBackend backend;
		CHECK(backend.Open("", AudioFreq::FREQ_48K, AudioSampleSize::FLOAT, AudioChannelCnt::STEREO, audio_channel_layout::automatic));
		backend.SetWriteCallback(produce_cb);
		backend.Play();

		std::atomic<bool> stop{false};
		std::thread other([&]
		{
			while (!stop)
			{
				backend.SetWriteCallback(produce_cb);
				backend.Open("", AudioFreq::FREQ_48K, AudioSampleSize::FLOAT, AudioChannelCnt::STEREO, audio_channel_layout::automatic);
				backend.Play();
			}
		});

		double worst_ms = 0;
		for (int i = 0; i < 100; i++)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			s_got_f32.clear();
			const auto t0 = std::chrono::steady_clock::now();
			libretro_audio_process(take_s16, 60.0);
			worst_ms = std::max(worst_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
		}
		stop = true;
		other.join();
		backend.Close();
		CHECK(worst_ms < 1.0);
		std::printf("test_audio_frontend: slowest run while cellAudio's thread churns: %.3f ms\n", worst_ms);
	}

	// retro_run with samples of the ring on loan while cellAudio's thread
	// opens, closes, sets a callback and ends and starts the lend: none of
	// it waits for the loan to be given back (it used to spin until it
	// was), and the loan, given back after, is harmless
	{
		CHECK(libretro_audio_negotiate_multi(environ_multi));
		LibretroAudioBackend backend;
		CHECK(backend.Open("", AudioFreq::FREQ_48K, AudioSampleSize::FLOAT, AudioChannelCnt::STEREO, audio_channel_layout::automatic));
		lr_audio_ring* ring = backend.LendRing(4800 * 2 * sizeof(float));
		CHECK(ring != nullptr);
		backend.Play();
		std::vector<float> block(800 * 2, 0.25f);
		CHECK(lr_audio_ring_push(ring, block.data(), block.size() * sizeof(float), false) == block.size() * sizeof(float));

		const void* ptr = nullptr;
		lr_audio_format format{};
		CHECK(backend.Borrow(&ptr, 800, &format) == 800);

		double worst_ms = 0;
		std::thread other([&]
		{
			const auto t0 = std::chrono::steady_clock::now();
			backend.EndLend();
			backend.Close();
			backend.SetWriteCallback(produce_cb);
			backend.Open("", AudioFreq::FREQ_48K, AudioSampleSize::FLOAT, AudioChannelCnt::STEREO, audio_channel_layout::automatic);
			backend.SetWriteCallback(nullptr);
			ring = backend.LendRing(4800 * 2 * sizeof(float));
			worst_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		});
		// The loan held for 50 ms
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		backend.Return(800);
		other.join();
		CHECK(worst_ms < 10.0);

		// The new stream after it: what is written now is what is heard
		backend.Play();
		std::vector<float> fresh(800 * 2, 0.5f);
		CHECK(lr_audio_ring_push(ring, fresh.data(), fresh.size() * sizeof(float), false) == fresh.size() * sizeof(float));
		s_got_f32.clear();
		libretro_audio_process(take_s16, 60.0);
		usz wrong = 0;
		for (float v : s_got_f32)
			wrong += v != 0.5f;
		CHECK(s_got_f32.size() == 800u * 2 && wrong == 0);
		backend.Close();
		std::printf("test_audio_frontend: cellAudio's thread reopened the stream in %.3f ms while retro_run held a loan for 50 ms\n", worst_ms);
	}

	// cellAudioOut's settings: the game configuring, cellAudio's reset
	// switching the output off and on and copy control set, all at once
	{
		audio_out_configuration::audio_out out;
		audio_out_configuration::audio_out_settings first{};
		first.sound_mode.channel = 2;
		first.sound_mode.type = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
		out.set_settings(first);

		constexpr int n = 200000;
		std::atomic<bool> go{false}, stop{false};
		std::atomic<u32> torn{0};
		const auto counter = [&](u32 audio_out_configuration::audio_out_settings::*field)
		{
			return std::thread([&, field]
			{
				while (!go) {}
				for (int i = 0; i < n; i++)
					out.change_settings([field](audio_out_configuration::audio_out_settings& s) { s.*field = (s.*field + 1) & 0xff; return true; });
			});
		};
		std::thread a = counter(&audio_out_configuration::audio_out_settings::downmixer);
		std::thread b = counter(&audio_out_configuration::audio_out_settings::copy_control);
		std::thread c([&]
		{
			while (!go) {}
			for (int i = 0; i < n; i++)
			{
				const u8 ch = (i & 1) ? 6 : 2;
				out.change_settings([ch](audio_out_configuration::audio_out_settings& s) { s.channels = ch; s.sound_mode.channel = ch; return true; });
			}
		});
		std::thread reader([&]
		{
			while (!go) {}
			while (!stop)
			{
				const auto s = out.settings();
				if (s.channels != s.sound_mode.channel)
					torn++;
			}
		});
		go = true;
		a.join();
		b.join();
		c.join();
		stop = true;
		reader.join();
		const auto last = out.settings();
		CHECK(last.downmixer == static_cast<u32>(n & 0xff));
		CHECK(last.copy_control == static_cast<u32>(n & 0xff));
		CHECK(torn == 0);
		std::printf("test_audio_frontend: cellAudioOut's settings: %d changes each from three threads, downmixer %u and copy control %u counted (%u expected), %u torn reads\n",
			n, last.downmixer, last.copy_control, n & 0xff, torn.load());
	}

	// cellMusicDecode's decoder: two WAV tracks, read while they decode
	{
		const std::string dir = fs::get_temp_dir() + "test_music_decode/";
		fs::create_path(dir);
		const u32 track_frames[2] = {150000, 90000};
		const auto sample = [](u32 track, u32 n) -> s16 { return static_cast<s16>(static_cast<s32>((n * (track ? 7u : 3u)) % 60000) - 30000); };
		std::vector<std::string> paths;
		for (u32 t = 0; t < 2; t++)
		{
			std::vector<u8> wav(44 + track_frames[t] * 4);
			const auto put32 = [&](usz at, u32 v) { std::memcpy(&wav[at], &v, 4); };
			const auto put16 = [&](usz at, u16 v) { std::memcpy(&wav[at], &v, 2); };
			std::memcpy(&wav[0], "RIFF", 4); put32(4, static_cast<u32>(wav.size() - 8)); std::memcpy(&wav[8], "WAVEfmt ", 8);
			put32(16, 16); put16(20, 1); put16(22, 2); put32(24, 48000); put32(28, 48000 * 4); put16(32, 4); put16(34, 16);
			std::memcpy(&wav[36], "data", 4); put32(40, track_frames[t] * 4);
			for (u32 n = 0; n < track_frames[t] * 2; n++)
				put16(44 + n * 2, static_cast<u16>(sample(t, n)));
			paths.push_back(dir + (t ? "b.wav" : "a.wav"));
			fs::write_file(paths.back(), fs::rewrite, wav);
		}

		utils::audio_decoder decoder;
		music_selection_context context{};
		context.playlist = paths;
		decoder.set_context(std::move(context));
		decoder.set_swap_endianness(false);
		decoder.decode();

		u32 track = 0, wrong = 0, reads = 0;
		u64 pos = 0, got[2] = {};
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		std::vector<f32> piece(4096);
		while (std::chrono::steady_clock::now() < until && !decoder.failed())
		{
			const u64 size = decoder.decoded_size();
			if (size > pos)
			{
				const u64 bytes = std::min<u64>(size - pos, piece.size() * sizeof(f32)) & ~u64{7};
				decoder.copy_out(reinterpret_cast<u8*>(piece.data()), pos, bytes);
				for (u64 i = 0; i < bytes / sizeof(f32); i++)
				{
					const u32 n = static_cast<u32>(pos / sizeof(f32) + i);
					if (std::fabs(piece[i] - sample(track, n) / 32768.0f) > 1e-6f)
						wrong++;
				}
				pos += bytes;
				got[track] = pos;
				reads++;
				continue;
			}
			if (decoder.track_decoded() && pos == size)
			{
				if (track == 1)
					break;
				CHECK(decoder.set_next_index(true) == 1);
				decoder.track_consumed();
				track = 1;
				pos = 0;
				continue;
			}
			std::this_thread::yield();
		}
		decoder.stop();
		fs::remove_file(paths[0]);
		fs::remove_file(paths[1]);
		CHECK(!decoder.failed());
		CHECK(got[0] == u64{track_frames[0]} * 8);
		CHECK(got[1] == u64{track_frames[1]} * 8);
		CHECK(wrong == 0);
		std::printf("test_audio_frontend: music decoder: %llu and %llu bytes of two tracks read in %u reads while decoding, %u samples wrong\n",
			static_cast<unsigned long long>(got[0]), static_cast<unsigned long long>(got[1]), reads, wrong);
	}

	std::printf("test_audio_frontend: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
