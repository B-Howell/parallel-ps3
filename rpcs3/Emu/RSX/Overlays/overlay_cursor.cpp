#include "stdafx.h"
#include "overlay_cursor.h"
#include "overlay_manager.h"
#include "Emu/Cell/timers.hpp"
#include "Emu/Memory/vm.h"
#include "Emu/RSX/gcm_enums.h"

namespace rsx
{
	extern u32 get_address(u32 offset, u32 location, u32 size_to_check = 0, std::source_location src_loc = std::source_location::current());

	namespace overlays
	{
		cursor_item::cursor_item()
		{
			m_cross_h.set_size(15, 1);
			m_cross_v.set_size(1, 15);
		}

		bool cursor_item::set_position(s16 x, s16 y)
		{
			if (m_x == x && m_y == y)
			{
				return false;
			}

			m_x = x;
			m_y = y;

			m_cross_h.set_pos(m_x - m_cross_h.w / 2, m_y - m_cross_h.h / 2);
			m_cross_v.set_pos(m_x - m_cross_v.w / 2, m_y - m_cross_v.h / 2);

			return true;
		}

		bool cursor_item::set_color(color4f color)
		{
			if (m_cross_h.back_color == color && m_cross_v.back_color == color)
			{
				return false;
			}

			m_cross_h.back_color = color;
			m_cross_h.refresh();

			m_cross_v.back_color = color;
			m_cross_v.refresh();

			return true;
		}

		void cursor_item::set_expiration(u64 expiration_time)
		{
			m_expiration_time = expiration_time;
		}

		bool cursor_item::update_visibility(u64 time)
		{
			m_visible = time <= m_expiration_time;

			return m_visible;
		}

		bool cursor_item::visible() const
		{
			return m_visible;
		}

		compiled_resource cursor_item::get_compiled()
		{
			if (!m_visible)
			{
				return {};
			}

			compiled_resource cr = m_cross_h.get_compiled();
			cr.add(m_cross_v.get_compiled());

			return cr;
		}

		static u8 color_channel(f32 v)
		{
			return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
		}

		void cursor_manager::take_requests()
		{
			for (u32 slot = 0; slot < cursor_offset::last; slot++)
			{
				const retro_u128_t word = retro_atomic_load_acquire_128(&m_requests[slot]);
				const u32 count = static_cast<u32>(word.hi >> 33);

				if (count == m_requests_seen[slot])
				{
					continue;
				}

				m_requests_seen[slot] = count;

				cursor_item& cursor = m_cursors[slot];

				const s16 x = static_cast<s16>(word.lo & 0xffff);
				const s16 y = static_cast<s16>((word.lo >> 16) & 0xffff);
				const color4f color =
				{
					static_cast<f32>((word.lo >> 32) & 0xff) / 255.0f,
					static_cast<f32>((word.lo >> 40) & 0xff) / 255.0f,
					static_cast<f32>((word.lo >> 48) & 0xff) / 255.0f,
					static_cast<f32>((word.lo >> 56) & 0xff) / 255.0f,
				};
				const u64 duration_us = static_cast<u32>(word.hi);
				const bool force_update = (word.hi >> 32) & 1;

				bool is_dirty = cursor.set_position(x, y);
				is_dirty |= cursor.set_color(color);

				if (is_dirty || force_update)
				{
					const u64 expiration_time = get_system_time() + duration_us;
					cursor.set_expiration(expiration_time);
					cursor.update_visibility(expiration_time);
				}
			}
		}

		void cursor_manager::update(u64 timestamp_us)
		{
			take_requests();

			if (!retro_atomic_load_acquire_int(&visible))
			{
				return;
			}

			bool any_cursor_visible = false;

			for (auto& entry : m_cursors)
			{
				any_cursor_visible |= entry.second.update_visibility(timestamp_us);
			}

			if (!any_cursor_visible)
			{
				retro_atomic_store_release_int(&visible, 0);
			}
		}

		compiled_resource cursor_manager::get_compiled()
		{
			take_requests();

			if (!retro_atomic_load_acquire_int(&visible))
			{
				return {};
			}

			compiled_resource cr{};

			for (auto& entry : m_cursors)
			{
				cr.add(entry.second.get_compiled());
			}

			return cr;
		}

		void cursor_manager::update_cursor(u32 id, s16 x, s16 y, const color4f& color, u64 duration_us, bool force_update)
		{
			if (id >= cursor_offset::last)
			{
				return;
			}

			// The request, for the render thread: the latest one counts
			const u64 lo = u64{static_cast<u16>(x)} | u64{static_cast<u16>(y)} << 16 |
				u64{color_channel(color.r)} << 32 | u64{color_channel(color.g)} << 40 |
				u64{color_channel(color.b)} << 48 | u64{color_channel(color.a)} << 56;

			for (retro_u128_t seen = retro_atomic_load_acquire_128(&m_requests[id]);;)
			{
				const u64 count = ((seen.hi >> 33) + 1) & 0x7fffffff;
				const retro_u128_t want{lo, count << 33 | u64{force_update} << 32 | std::min<u64>(duration_us, 0xffffffff)};

				if (retro_atomic_cas_seen_128(&m_requests[id], &seen, want))
				{
					break;
				}
			}

			// Drawn, so that the render thread looks; it works out whether it stays
			retro_atomic_store_release_int(&visible, 1);
		}

		void set_cursor(u32 id, s16 x, s16 y, const color4f& color, u64 duration_us, bool force_update)
		{
			if (auto manager = g_fxo->try_get<rsx::overlays::display_manager>())
			{
				auto cursor_overlay = manager->get<rsx::overlays::cursor_manager>();
				if (!cursor_overlay)
				{
					cursor_overlay = std::make_shared<rsx::overlays::cursor_manager>();
					cursor_overlay = manager->add(cursor_overlay);
				}
				cursor_overlay->update_cursor(id, x, y, color, duration_us, force_update);
			}
		}

		void bitmap_cursor::enable()
		{
			retro_atomic_fetch_or_64(&m_place, s64{1} << 32);
		}

		void bitmap_cursor::disable()
		{
			retro_atomic_fetch_and_64(&m_place, ~(s64{1} << 32));
		}

		void bitmap_cursor::set_screen_size(u16 w, u16 h)
		{
			retro_atomic_store_release_int(&m_size, static_cast<int>(u32{w} | u32{h} << 16));
		}

		void bitmap_cursor::set_pos(s32 x, s32 y)
		{
			const u64 xy = u64{static_cast<u16>(::narrow<s16>(x))} | u64{static_cast<u16>(::narrow<s16>(y))} << 16;

			for (s64 seen = retro_atomic_load_acquire_64(&m_place);;)
			{
				const s64 want = static_cast<s64>((static_cast<u64>(seen) & ~u64{0xffffffff}) | xy);

				if (want == seen || retro_atomic_cas_64(&m_place, seen, want))
				{
					break;
				}

				seen = retro_atomic_load_acquire_64(&m_place);
			}
		}

		void bitmap_cursor::set_bitmap(u32 address)
		{
			// Made into an image by the render thread
			for (s64 seen = retro_atomic_load_acquire_64(&m_image);;)
			{
				const u64 count = (static_cast<u64>(seen) >> 32) + 1;

				if (retro_atomic_cas_64(&m_image, seen, static_cast<s64>(count << 32 | address)))
				{
					break;
				}

				seen = retro_atomic_load_acquire_64(&m_image);
			}
		}

		compiled_resource bitmap_cursor::get_compiled()
		{
			const u64 image = static_cast<u64>(retro_atomic_load_acquire_64(&m_image));
			const u64 place = static_cast<u64>(retro_atomic_load_acquire_64(&m_place));

			bool count_changed = false;

			if (const u32 count = static_cast<u32>(image >> 32); count != m_image_seen)
			{
				count_changed = true;
				m_image_seen = count;

				if (!m_bitmap)
				{
					m_bitmap = std::make_unique<image_view>();
					m_bitmap->set_size(64, 64);
				}

				auto vm_address = rsx::get_address(static_cast<u32>(image), CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
				auto data = vm::get_super_ptr<const u8>(vm_address);
				m_image_storage = { 64, 64, 4, data };
				m_bitmap->set_raw_image(&m_image_storage);
			}

			if (!((place >> 32) & 1) || !m_bitmap)
			{
				return {};
			}

			// Moved only when it was moved, so it is not compiled again each frame
			if (const u32 xy = static_cast<u32>(place); xy != m_place_seen || count_changed)
			{
				m_place_seen = xy;
				m_bitmap->set_pos(static_cast<s16>(xy & 0xffff), static_cast<s16>(xy >> 16));
			}

			return m_bitmap->get_compiled();
		}
	} // namespace overlays
} // namespace rsx
