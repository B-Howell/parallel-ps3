#pragma once

#include <retro_atomic.h>

#include "overlays.h"
#include "Utilities/mutex.h"
#include <map>

namespace rsx
{
	namespace overlays
	{
		enum cursor_offset : u32
		{
			cell_gem = 0, // CELL_GEM_MAX_NUM = 4 Move controllers
			last = 4
		};

		class cursor_item
		{
		public:
			cursor_item();

			void set_expiration(u64 expiration_time);
			bool set_position(s16 x, s16 y);
			bool set_color(color4f color);

			bool update_visibility(u64 time);
			bool visible() const;

			compiled_resource get_compiled();

		private:
			bool m_visible = false;
			overlay_element m_cross_h{};
			overlay_element m_cross_v{};
			u64 m_expiration_time = 0;
			s16 m_x = 0;
			s16 m_y = 0;
		};

		class cursor_manager final : public overlay
		{
		public:
			void update(u64 timestamp_us) override;
			compiled_resource get_compiled() override;

			void update_cursor(u32 id, s16 x, s16 y, const color4f& color, u64 duration_us, bool force_update);

		private:
			// The cursors are the render thread's (update and get_compiled run
			// there). update_cursor, from any thread, leaves its request in the
			// cursor's slot - one 128-bit word: x, y, the colour as 8-bit
			// channels, the duration, a count of requests and the force flag -
			// which the render thread takes up. No lock
			void take_requests();

			retro_atomic_128_t m_requests[cursor_offset::last]{};
			u32 m_requests_seen[cursor_offset::last]{};
			std::map<u32, cursor_item> m_cursors;
		};

		class bitmap_cursor final : public overlay
		{
		public:
			void enable();
			void disable();
			void set_pos(s32 x, s32 y);
			void set_bitmap(u32 address);
			void set_screen_size(u16 w, u16 h);

			compiled_resource get_compiled() override;
			u16 get_virtual_width() const override { return static_cast<u16>(retro_atomic_load_acquire_int(&m_size)); }
			u16 get_virtual_height() const override { return static_cast<u16>(static_cast<u32>(retro_atomic_load_acquire_int(&m_size)) >> 16); }

		private:
			// Set by sys_rsx's calls from any thread, read by the render thread:
			// words, no lock. x and y (16 bits each) and visible (bit 32)
			retro_atomic_64_t m_place{};
			// The screen's width and height, 16 bits each
			mutable retro_atomic_int_t m_size{static_cast<int>(1280u | 720u << 16)};
			// The bitmap's address and a count of set_bitmap's (high half)
			retro_atomic_64_t m_image{};

			// The render thread's
			u32 m_image_seen = 0;
			u32 m_place_seen = ~0u; // the x and y the bitmap was put at
			std::unique_ptr<overlays::image_view> m_bitmap;
			overlays::memory_image_info m_image_storage;
		};

		void set_cursor(u32 id, s16 x, s16 y, const color4f& color, u64 duration_us, bool force_update);

	} // namespace overlays
} // namespace rsx
