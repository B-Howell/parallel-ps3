#include "stdafx.h"
#include "overlay_trophy_notification.h"
#include "Emu/Cell/ErrorCodes.h"

namespace rsx
{
	namespace overlays
	{
		// TODO: Move somewhere in rsx_utils or general utils if needed anywhere else
		class ticket_semaphore_t
		{
			retro_atomic_64_t acquired{0};
			retro_atomic_64_t released{0};

		public:
			u64 enqueue()
			{
				return static_cast<u64>(retro_atomic_fetch_add_64(&acquired, 1));
			}

			bool try_acquire(u64 queue_id) const
			{
				return (queue_id == static_cast<u64>(retro_atomic_load_acquire_64(&released)));
			}

			void release()
			{
				retro_atomic_fetch_add_64(&released, 1);
			}
		};

		static ticket_semaphore_t s_trophy_semaphore;

		trophy_notification::trophy_notification()
		{
			frame.set_pos(68, 55);
			frame.set_size(300, 72);
			frame.back_color.r = 0.247059f;
			frame.back_color.g = 0.250980f;
			frame.back_color.b = 0.247059f;
			frame.back_color.a = 0.88f;

			image.set_pos(78, 64);
			image.set_size(53, 53);
			image.back_color.a = 0.f;

			text_view.set_pos(139, 69);
			text_view.set_padding(0, 0, 0, 0);
			text_view.set_font(14);
			text_view.align_text(overlay_element::text_align::center);
			text_view.back_color.a = 0.f;

			sliding_animation.duration_sec = 1.5f;
			sliding_animation.type = animation_type::ease_in_out_cubic;

			// Make the fade animation a bit shorter to see the trophy better.
			fade_animation.duration_sec = 1.0f;
		}

		void trophy_notification::update(u64 timestamp_us)
		{
			if (!s_trophy_semaphore.try_acquire(display_sched_id))
			{
				// Not scheduled to run just yet
				return;
			}

			if (!creation_time_us)
			{
				// First tick
				creation_time_us = timestamp_us;
				play_sound(sound_effect::trophy);
				return;
			}

			const u64 time_since_creation_us = timestamp_us - creation_time_us;
			u64 end_animation_begin_us = 5'000'000;

			if (time_since_creation_us > end_animation_begin_us)
			{
				if (!sliding_animation.active)
				{
					sliding_animation.end = { -f32(frame.x + frame.w), 0, 0 };
					sliding_animation.on_finish = [this]
					{
						s_trophy_semaphore.release();
						close(false, false);
					};

					sliding_animation.active = true;
				}
			}

			// Match both animation ends based on their durations
			if (sliding_animation.duration_sec > fade_animation.duration_sec)
			{
				end_animation_begin_us += static_cast<u64>((sliding_animation.duration_sec - fade_animation.duration_sec) * 1'000'000);
			}

			if (time_since_creation_us > end_animation_begin_us)
			{
				if (!fade_animation.active)
				{
					fade_animation.current = color4f(1.f);
					fade_animation.end = color4f(0.f);

					fade_animation.active = true;
				}
			}

			if (sliding_animation.active)
			{
				sliding_animation.update(timestamp_us);
			}

			if (fade_animation.active)
			{
				fade_animation.update(timestamp_us);
			}
		}

		compiled_resource trophy_notification::get_compiled()
		{
			if (!creation_time_us || !retro_atomic_load_acquire_int(&visible))
			{
				return {};
			}

			auto result = frame.get_compiled();
			result.add(image.get_compiled());
			result.add(text_view.get_compiled());

			sliding_animation.apply(result);
			fade_animation.apply(result);

			return result;
		}

		s32 trophy_notification::show(const SceNpTrophyDetails& trophy, const std::vector<uchar>& trophy_icon_buffer)
		{
			// Schedule to display this trophy
			display_sched_id = s_trophy_semaphore.enqueue();
			retro_atomic_store_release_int(&visible, 0);

			if (!trophy_icon_buffer.empty())
			{
				icon_info = std::make_unique<image_info>(trophy_icon_buffer);
				image.set_raw_image(icon_info.get());
			}

			localized_string_id string_id = localized_string_id::INVALID;
			switch (trophy.trophyGrade)
			{
			case SCE_NP_TROPHY_GRADE_BRONZE: string_id = localized_string_id::RSX_OVERLAYS_TROPHY_BRONZE; break;
			case SCE_NP_TROPHY_GRADE_SILVER: string_id = localized_string_id::RSX_OVERLAYS_TROPHY_SILVER; break;
			case SCE_NP_TROPHY_GRADE_GOLD: string_id = localized_string_id::RSX_OVERLAYS_TROPHY_GOLD; break;
			case SCE_NP_TROPHY_GRADE_PLATINUM: string_id = localized_string_id::RSX_OVERLAYS_TROPHY_PLATINUM; break;
			default: break;
			}

			text_view.set_unicode_text(get_localized_u32string(string_id, trophy.name));
			text_view.auto_resize();

			// Resize background to cover the text
			constexpr u16 margin_sz = 9;
			frame.w = margin_sz * 3 + image.w + text_view.w;

			sliding_animation.current = { -f32(frame.x + frame.w), 0, 0 };
			sliding_animation.end = { 0, 0, 0 };
			sliding_animation.active = true;

			fade_animation.current = color4f(0.f);
			fade_animation.end = color4f(1.f);
			fade_animation.active = true;

			retro_atomic_store_release_int(&visible, 1);

			return CELL_OK;
		}
	} // namespace overlays
} // namespace rsx
