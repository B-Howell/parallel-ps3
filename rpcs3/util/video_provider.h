#pragma once

#include <retro_atomic.h>

#include "video_sink.h"

#include <retro_atomic.h>

#include <memory>

enum class recording_mode
{
	stopped = 0,
	rpcs3,
	cell
};

namespace utils
{
	class video_provider
	{
	public:
		video_provider();
		~video_provider();

		bool set_video_sink(std::shared_ptr<video_sink> sink, recording_mode type);
		void set_pause_time_us(usz pause_time_us);

		bool can_consume_frame();
		void present_frame(std::vector<u8>&& data, u32 pitch, u32 width, u32 height, bool is_bgra);

		void present_samples(const u8* buf, u32 sample_count, u16 channels);

	private:
		recording_mode check_mode(video_sink& sink);

		// The sink, for the renderer's and cellAudio's threads to use without
		// a lock: each counts itself in before it reads the pointer and out
		// when it is done, and set_video_sink() takes a sink out of use only
		// once the count it was published under is down to zero
		struct sink_use
		{
			video_provider& owner;
			retro_atomic_int_t& users;
			video_sink* sink;

			sink_use(video_provider& o, retro_atomic_int_t& u);
			~sink_use();
		};

		// set_video_sink()'s (the PPU thread's) alone
		recording_mode m_type = recording_mode::stopped;
		std::shared_ptr<video_sink> m_video_sink;

		retro_atomic_ptr_t m_sink;             // the video_sink*, published
		retro_atomic_int_t m_video_users;
		retro_atomic_int_t m_audio_users;
		retro_atomic_size_t m_generation;      // moves on with each new sink
		retro_atomic_size_t m_pause_time_us;
		retro_atomic_int_t m_active{0}; // 0 or 1
		retro_atomic_64_t m_start_time_us{-1}; // usz, umax until the first frame or samples

		bool active() const { return retro_atomic_load_acquire_int(&m_active) != 0; }
		usz start_time_us() const { return static_cast<usz>(retro_atomic_load_acquire_64(&m_start_time_us)); }

		// The renderer's thread's, and cellAudio's: the last time handed
		// over, reset when the sink is a new one
		s64 m_last_video_pts_incoming = -1;
		usz m_video_generation = 0;
		s64 m_last_audio_pts_incoming = -1;
		usz m_audio_generation = 0;
	};

} // namespace utils
