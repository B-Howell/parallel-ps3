#include "stdafx.h"
#include "media_utils.h"
#include "media_decode.h"
#include "media_encode.h"
#include "Emu/emu_callbacks.h"

#include <formats/rtags.h>

#include <random>
#include <rthreads/retro_waitaddr.h>
#include <thread>


LOG_CHANNEL(media_log, "Media");

namespace utils
{
	template <typename T>
	static inline void write_byteswapped(const void* src, void* dst)
	{
		*reinterpret_cast<T*>(dst) = *reinterpret_cast<const be_t<T>*>(src);
	}

	template <typename T>
	static inline void copy_samples(const u8* src, u8* dst, usz sample_count, bool swap_endianness)
	{
		if (swap_endianness)
		{
			for (usz i = 0; i < sample_count; i++)
			{
				write_byteswapped<T>(src + i * sizeof(T), dst + i * sizeof(T));
			}
		}
		else
		{
			std::memcpy(dst, src, sample_count * sizeof(T));
		}
	}

	template <typename T>
	T media_info::get_metadata([[maybe_unused]] const std::string& key, const T& def) const
	{
		static_assert("unimplemented");
		return def;
	}

	template <>
	std::string media_info::get_metadata(const std::string& key, const std::string& def) const
	{
		if (metadata.contains(key))
		{
			return ::at32(metadata, key);
		}

		return def;
	}

	template <>
	s64 media_info::get_metadata(const std::string& key, const s64& def) const
	{
		if (metadata.contains(key))
		{
			s64 result{};
			if (try_to_int64(&result, ::at32(metadata, key), smin, smax))
			{
				return result;
			}
		}

		return def;
	}

	std::pair<bool, media_info> get_media_info(const std::string& path, s32 av_media_type)
	{
		media_info info{};
		info.path = path;

		if (av_media_type == -1) // Let's use this for image info
		{
			const bool success = g_emu_callbacks.get_image_info(path, info.sub_type, info.width, info.height, info.orientation);
			if (!success) media_log.error("get_media_info: failed to get image info for '%s'", path);
			return { success, std::move(info) };
		}

		md_file file;
		if (!md_file_open(&file, path.c_str()))
		{
			media_log.notice("get_media_info: could not open file '%s'", path);
			return { false, std::move(info) };
		}

		md_info mi;
		if (!md_probe(file.data, file.len, av_media_type == 0, &mi))
		{
			md_file_close(&file);
			media_log.notice("get_media_info: '%s' is not a %s file read here", path, av_media_type == 0 ? "video" : "music");
			return { false, std::move(info) };
		}

		info.audio_codec = mi.audio_codec;
		info.video_codec = mi.video_codec;
		info.audio_bitrate_bps = mi.audio_bitrate;
		info.video_bitrate_bps = mi.video_bitrate;
		info.sample_rate = mi.sample_rate;
		info.duration_us = mi.duration_us;

		// The tags, under the keys FFmpeg gave them
		static const struct { rtags_field field; const char* key; } tags[] =
		{
			{ RTAGS_TITLE, "title" }, { RTAGS_ARTIST, "artist" }, { RTAGS_ALBUM, "album" },
			{ RTAGS_GENRE, "genre" }, { RTAGS_TRACK, "track" }, { RTAGS_DISC, "disc" },
			{ RTAGS_DATE, "date" },
		};
		char value[1024];
		for (const auto& tag : tags)
		{
			if (rtags_get(file.data, file.len, tag.field, value, sizeof(value)))
			{
				info.metadata[tag.key] = value;
			}
		}

		md_file_close(&file);
		return { true, std::move(info) };
	}

	audio_decoder::audio_decoder()
		: m_md(md_audio_new()), m_pcm(MD_READ_MIN * 2)
	{
		retro_atomic_size_init(&m_size, 0);
		retro_atomic_size_init(&m_time_count, 0);
		retro_atomic_int_init(&m_track, 0);
		retro_atomic_int_init(&m_track_decoded, 0);
		retro_atomic_int_init(&m_error, 0);
		retro_atomic_int_init(&m_consumed, 0);
		retro_atomic_int_init(&m_stop, 0);
	}

	audio_decoder::~audio_decoder()
	{
		stop();
		md_audio_free(m_md);
	}

	void audio_decoder::set_context(music_selection_context&& context)
	{
		m_context = std::move(context);
	}

	void audio_decoder::set_swap_endianness(bool swapped)
	{
		m_swap_endianness = swapped;
	}

	// With the thread stopped: nothing decoded, nothing consumed. The
	// chunks are kept for the next playlist
	void audio_decoder::clear()
	{
		media_log.notice("audio_decoder: Clear data...");

		retro_atomic_store_release_size(&m_size, 0);
		retro_atomic_store_release_size(&m_time_count, 0);
		retro_atomic_store_release_int(&m_track, 0);
		retro_atomic_store_release_int(&m_track_decoded, 0);
		retro_atomic_store_release_int(&m_error, 0);
		retro_atomic_store_release_int(&m_consumed, 0);
		m_consumed_track = 0;
		m_time_read = 0;
	}

	void audio_decoder::stop()
	{
		media_log.notice("audio_decoder: Stop decoding...");

		if (m_thread)
		{
			auto& thread = *m_thread;
			retro_atomic_store_release_int(&m_stop, 1);
			retro_waitaddr_notify_all(&m_consumed);
			thread();
			m_thread.reset();
			retro_atomic_store_release_int(&m_stop, 0);
		}

		clear();
	}

	u64 audio_decoder::decoded_size() const
	{
		// None of the track it is done with, while the thread has not gone
		// on to the next
		if (retro_atomic_load_acquire_int(const_cast<retro_atomic_int_t*>(&m_track)) == m_consumed_track)
			return 0;
		return retro_atomic_load_acquire_size(const_cast<retro_atomic_size_t*>(&m_size));
	}

	bool audio_decoder::track_decoded() const
	{
		return retro_atomic_load_acquire_int(const_cast<retro_atomic_int_t*>(&m_track)) != m_consumed_track
			&& retro_atomic_load_acquire_int(const_cast<retro_atomic_int_t*>(&m_track_decoded));
	}

	bool audio_decoder::failed() const
	{
		return retro_atomic_load_acquire_int(const_cast<retro_atomic_int_t*>(&m_error)) != 0;
	}

	void audio_decoder::copy_out(u8* dst, u64 pos, u64 size) const
	{
		while (size)
		{
			const u64 in_chunk = std::min<u64>(size, chunk_size - pos % chunk_size);
			std::memcpy(dst, m_chunks[pos / chunk_size].get() + pos % chunk_size, in_chunk);
			dst += in_chunk;
			pos += in_chunk;
			size -= in_chunk;
		}
	}

	u64 audio_decoder::time_at(u64 pos)
	{
		const u64 count = retro_atomic_load_acquire_size(&m_time_count);
		if (m_time_read >= count)
			return 0;
		while (m_time_read + 1 < count && pos >= m_times[m_time_read + 1].first)
			m_time_read++;
		return m_times[m_time_read].second;
	}

	u32 audio_decoder::set_next_index(bool next)
	{
		// The thread waits for the track to be consumed before it looks at
		// the context again
		return m_context.step_track(next);
	}

	void audio_decoder::track_consumed()
	{
		m_consumed_track = retro_atomic_load_acquire_int(&m_track);
		m_time_read = 0;
		retro_atomic_store_release_int(&m_consumed, 1);
		retro_waitaddr_notify_all(&m_consumed);
	}

	void audio_decoder::decode()
	{
		stop();

		media_log.notice("audio_decoder: %d entries in playlist. Start decoding...", m_context.playlist.size());

		const auto stopping = [this]()
		{
			return retro_atomic_load_acquire_int(&m_stop) || thread_ctrl::state() == thread_state::aborting;
		};

		const auto decode_track = [this, stopping](const std::string& path)
		{
			media_log.notice("audio_decoder: decoding %s", path);

			md_file file;
			if (!m_md || !md_file_open(&file, path.c_str()))
			{
				media_log.error("audio_decoder: Could not open file '%s'", path);
				retro_atomic_store_release_int(&m_error, 1);
				return;
			}

			// Any format audio_transfer plays, folded to stereo, at 48 kHz
			if (!md_audio_open(m_md, file.data, file.len))
			{
				media_log.error("audio_decoder: '%s' is not in a format that can be played", path);
				md_file_close(&file);
				retro_atomic_store_release_int(&m_error, 1);
				return;
			}

			constexpr u64 bytes_per_frame = 2 * sizeof(f32);
			u64 size = 0;
			u64 times = 0;

			while (!stopping())
			{
				u64 timestamp_ms = 0;
				const usz frames = md_audio_read(m_md, m_pcm.data(), m_pcm.size() / 2, &timestamp_ms);
				if (!frames)
				{
					break;
				}

				const u64 block = frames * bytes_per_frame;
				u8* out = reinterpret_cast<u8*>(m_pcm.data());

				// The format is float 32bit per channel. Swap it in place, so we can append the buffer as-is.
				if (m_swap_endianness)
				{
					copy_samples<f32>(out, out, frames * 2, true);
				}

				if (size + block > chunk_size * max_chunks)
				{
					media_log.error("audio_decoder: '%s' is longer than %d MiB decoded; the rest is not played", path, chunk_size * max_chunks >> 20);
					break;
				}

				// Into the chunks, made as they are first needed; then
				// published, its time first
				for (u64 at = 0; at < block;)
				{
					auto& chunk = m_chunks[(size + at) / chunk_size];
					if (!chunk)
						chunk = std::make_unique<u8[]>(chunk_size);
					const u64 in_chunk = std::min<u64>(block - at, chunk_size - (size + at) % chunk_size);
					std::memcpy(chunk.get() + (size + at) % chunk_size, out + at, in_chunk);
					at += in_chunk;
				}

				if (times < max_times)
				{
					m_times[times++] = media_timestamp{size, static_cast<u64>(timestamp_ms)};
					retro_atomic_store_release_size(&m_time_count, times);
				}

				size += block;
				retro_atomic_store_release_size(&m_size, size);

				media_log.trace("audio_decoder: decoded frame_count=%d size=%d timestamp_ms=%d", frames, block, timestamp_ms);
			}

			if (md_audio_failed(m_md))
			{
				media_log.error("audio_decoder: Decoding error in '%s'", path);
				retro_atomic_store_release_int(&m_error, 1);
			}

			md_audio_close(m_md);
			md_file_close(&file);
		};

		m_thread = std::make_unique<named_thread<std::function<void()>>>("Music Decode Thread", [this, decode_track, stopping]()
		{
			for (const std::string& track : m_context.playlist)
			{
				media_log.notice("audio_decoder: playlist entry: %s", track);
			}

			if (m_context.playlist.empty())
			{
				media_log.error("audio_decoder: Can not play empty playlist");
				retro_atomic_store_release_int(&m_error, 1);
				return;
			}

			if (m_context.context_option == CELL_SEARCH_CONTEXTOPTION_SHUFFLE && m_context.playlist.size() > 1)
			{
				// Shuffle once if necessary
				media_log.notice("audio_decoder: shuffling initial playlist...");
				// Seeded from the clock: std::random_device keeps its state
				// on the stack, 5 KiB, past the core's 4 KiB stack frame
				auto engine = std::default_random_engine{static_cast<std::default_random_engine::result_type>(std::chrono::steady_clock::now().time_since_epoch().count())};
				std::shuffle(std::begin(m_context.playlist), std::end(m_context.playlist), engine);
			}

			while (!stopping())
			{
				media_log.notice("audio_decoder: about to decode: %s (index=%d)", ::at32(m_context.playlist, m_context.current_track), m_context.current_track);

				// A new track: nothing of it yet, then its number, which
				// the consumer goes by
				retro_atomic_store_release_size(&m_size, 0);
				retro_atomic_store_release_size(&m_time_count, 0);
				retro_atomic_store_release_int(&m_track_decoded, 0);
				retro_atomic_store_release_int(&m_consumed, 0);
				retro_atomic_store_release_int(&m_track, retro_atomic_load_acquire_int(&m_track) + 1);

				decode_track(::at32(m_context.playlist, m_context.current_track));
				retro_atomic_store_release_int(&m_track_decoded, 1);

				if (failed())
				{
					media_log.notice("audio_decoder: stopping with error...");
					break;
				}

				// Let's only decode one track at a time. Wait for the consumer to finish reading the track.
				media_log.notice("audio_decoder: waiting until track is consumed...");

				while (!stopping() && !retro_atomic_load_acquire_int(&m_consumed))
				{
					const retro_waitaddr_t list[2] =
					{
						{&m_consumed, 0},
						{&m_stop, 0},
					};
					retro_waitaddr_wait(list, 2, -1);
				}
			}

			media_log.notice("audio_decoder: finished playlist");
		});
	}

	video_encoder::video_encoder()
		: utils::video_sink()
	{
	}

	video_encoder::~video_encoder()
	{
		stop();
	}

	std::string video_encoder::path() const
	{
		return m_path;
	}

	s64 video_encoder::last_video_pts() const
	{
		return m_last_video_pts;
	}

	void video_encoder::set_path(const std::string& path)
	{
		m_path = path;
	}

	void video_encoder::set_framerate(u32 framerate)
	{
		m_framerate = framerate;
	}

	void video_encoder::set_video_bitrate(u32 bitrate)
	{
		m_video_bitrate_bps = bitrate;
	}

	void video_encoder::set_output_format(video_encoder::frame_format format)
	{
		m_out_format = std::move(format);
	}

	void video_encoder::set_video_codec(s32 codec)
	{
		m_video_codec = codec;
	}

	void video_encoder::set_gop_size(s32 gop_size)
	{
		m_gop_size = gop_size;
	}

	void video_encoder::set_sample_rate(u32 sample_rate)
	{
		m_sample_rate = sample_rate;
	}

	void video_encoder::set_audio_channels(u32 channels)
	{
		m_channels = channels;
	}

	void video_encoder::set_audio_bitrate(u32 bitrate)
	{
		m_audio_bitrate_bps = bitrate;
	}

	void video_encoder::set_audio_codec(s32 codec)
	{
		m_audio_codec = codec;
	}

	void video_encoder::pause(bool flush)
	{
		if (m_thread)
		{
			retro_atomic_store_release_int(&m_paused, 1);
			retro_atomic_store_release_int(&m_flush, (flush) ? 1 : 0);
			wake_consumer();

			if (flush)
			{
				// Woken as the encoder has flushed, or ended (it slept 1 us at
				// a time before)
				for (u32 seen = freed_seen(); retro_atomic_load_acquire_int(&m_flush) && m_running; seen = freed_seen())
				{
					wait_freed(seen);
				}
			}
		}
	}

	void video_encoder::stop(bool flush)
	{
		media_log.notice("video_encoder: Stopping video encoder. flush=%d", flush);

		if (m_thread)
		{
			retro_atomic_store_release_int(&m_flush, (flush) ? 1 : 0);
			wake_consumer();

			if (flush)
			{
				// As in pause (it slept 1 ms at a time)
				for (u32 seen = freed_seen(); retro_atomic_load_acquire_int(&m_flush) && m_running; seen = freed_seen())
				{
					wait_freed(seen);
				}
			}

			// Join thread
			m_thread.reset();
		}

		clear_queues();
		retro_atomic_store_release_int(&has_error, 0);
		retro_atomic_store_release_int(&m_flush, 0);
		retro_atomic_store_release_int(&m_paused, 0);
		m_running = false;
	}

	void video_encoder::resume()
	{
		media_log.notice("video_encoder: Resuming video encoder");

		retro_atomic_store_release_int(&m_flush, 0);
		retro_atomic_store_release_int(&m_paused, 0);
	}

	void video_encoder::encode()
	{
		if (m_running)
		{
			// Resume
			resume();
			media_log.success("video_encoder: resuming recording of '%s'", m_path);
			return;
		}

		m_last_audio_pts = 0;
		m_last_video_pts = 0;

		stop();

		if (const std::string dir = fs::get_parent_dir(m_path); !fs::is_dir(dir))
		{
			media_log.error("video_encoder: Could not find directory: '%s' for file '%s'", dir, m_path);
			set_error();
			return;
		}

		// The audio is encoded at 48 kHz whatever cellRec asks for, as FFmpeg
		// chose for each of its formats; the samples come at that rate
		m_sample_rate = 48000;

		media_log.success("video_encoder: Starting recording of '%s'", m_path);

		m_thread = std::make_unique<named_thread<std::function<void()>>>("Video Encode Thread", [this, path = m_path]()
		{
			m_running = true;

			me_params params{};
			params.path = path.c_str();
			params.video_codec = m_video_codec;
			params.width = m_out_format.width;
			params.height = m_out_format.height;
			params.fps = m_framerate;
			params.video_bitrate = m_video_bitrate_bps;
			params.gop = static_cast<unsigned>(m_gop_size);
			params.audio_codec = m_audio_codec;
			params.sample_rate = m_sample_rate;
			params.channels = m_channels;
			params.audio_bitrate = m_audio_bitrate_bps;
			params.max_seconds = max_seconds;

			media_log.notice("video_encoder: video codec %d at %dx%d, %d fps, %d bit/s, gop %d; audio codec %d, %d Hz, %d channels, %d bit/s",
				m_video_codec, m_out_format.width, m_out_format.height, m_framerate, m_video_bitrate_bps, m_gop_size,
				m_audio_codec, m_sample_rate, m_channels, m_audio_bitrate_bps);

			me_encoder* const enc = me_open(&params);

			if (!enc)
			{
				media_log.error("video_encoder: Could not start '%s'", path);
				set_error();
				retro_atomic_store_release_int(&m_flush, 0);
				m_running = false;
				wake_producer();
				return;
			}

			s64 last_video_pts = -1;
			u64 audio_written = 0; // frames, silence included

			// Its buffer goes back to the producer with the next frame
			encoder_frame frame_data;

			while ((thread_ctrl::state() != thread_state::aborting || retro_atomic_load_acquire_int(&m_flush)) && !retro_atomic_load_acquire_int(&has_error))
			{
				// Read before looking: anything handed over, or asked, after
				// this moves it, and the wait below returns
				const u32 filled = filled_seen();

				// Fetch video frame
				const bool got_frame = pop_frame(frame_data);

				if (got_frame)
				{
					// Calculate presentation timestamp.
					const s64 pts = get_pts(frame_data.timestamp_ms);

					// We need to skip this frame if it has the same timestamp.
					if (pts <= last_video_pts)
					{
						media_log.trace("video_encoder: skipping frame. last_pts=%d, pts=%d, timestamp_ms=%d", last_video_pts, pts, frame_data.timestamp_ms);
					}
					else
					{
						const u32 pitch = frame_data.pitch ? frame_data.pitch : frame_data.width * 4;

						if (!me_video(enc, frame_data.data.data(), pitch, frame_data.width, frame_data.height, frame_data.pixel_format, pts))
						{
							media_log.error("video_encoder: Could not write a frame (%dx%d, format %d)", frame_data.width, frame_data.height, frame_data.pixel_format);
							set_error();
							break;
						}

						last_video_pts = pts;
						m_last_video_pts = pts;
					}
				}

				// Fetch audio sample
				const encoder_sample* const sample = peek_sample();
				const bool got_sample = sample != nullptr;

				if (got_sample)
				{
					if (sample->channels != m_channels)
					{
						media_log.error("video_encoder: Audio sample channel count %d does not match the recording's %d", sample->channels, m_channels);
						set_error();
						break;
					}

					// Where this block starts, in frames
					const s64 pts = get_audio_pts(sample->timestamp_us);
					const u64 start = static_cast<u64>(std::max<s64>(pts, 0)) * m_samples_per_block;

					// We need to skip this block if its time is already written.
					if (start < audio_written)
					{
						media_log.trace("video_encoder: skipping sample. written=%d, pts=%d, timestamp_us=%d", audio_written, pts, sample->timestamp_us);
					}
					else
					{
						// Silence to fill the gap, then the samples
						if ((start > audio_written && !me_audio(enc, nullptr, start - audio_written)) ||
							!me_audio(enc, sample->data.data(), sample->sample_count))
						{
							media_log.error("video_encoder: Could not write audio");
							set_error();
							break;
						}

						audio_written = start + sample->sample_count;
						m_last_audio_pts = static_cast<s64>(audio_written / m_samples_per_block);
					}
					release_sample();
				}


				if (!got_frame && !got_sample)
				{
					if (retro_atomic_load_acquire_int(&m_flush))
					{
						retro_atomic_store_release_int(&m_flush, 0);
						wake_producer();

						if (!retro_atomic_load_acquire_int(&m_paused))
						{
							// We only stop the thread after a flush if we are not paused
							break;
						}
					}

					// Nothing to take: woken by the next frame or block, or a
					// pause, flush or stop asked - it slept 1 us (10 ms paused)
					// at a time before
					// (also as the thread is told to end)
					thread_ctrl::wait_on_custom<1>([&](atomic_wait::list<3>& list)
					{
						list.template set<0>(*utils::bless<atomic_t<u32>>(&m_filled), filled);
					});
					continue;
				}
			}

			if (!me_close(enc))
			{
				media_log.error("video_encoder: Could not finish '%s'", path);
			}

			retro_atomic_store_release_int(&m_flush, 0);
			m_running = false;
			wake_producer();
		});
	}
}
