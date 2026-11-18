#include "stdafx.h"
#include "overlay_home_icons.h"

#include "Emu/RSX/Overlays/overlay_controls.h"

#include <retro_atomic.h>

namespace rsx::overlays::home_menu
{
	// One slot per icon, loaded once: the first loader to publish it
	// (compare-and-swap) wins, a loser frees its own - no lock
	static constexpr usz icon_count = static_cast<usz>(fa_icon::bug) + 1;

	struct icons_cache_t
	{
		retro_atomic_ptr_t slots[icon_count]{};

		~icons_cache_t()
		{
			for (auto& slot : slots)
			{
				delete static_cast<image_info*>(retro_atomic_load_acquire_ptr(&slot));
			}
		}
	};

	static icons_cache_t g_icons_cache;

	static const char* fa_icon_to_filename(fa_icon icon)
	{
		switch (icon)
		{
		default:
		case fa_icon::none:
			return "";
		case fa_icon::home:
			return "home.png";
		case fa_icon::settings:
			return "settings.png";
		case fa_icon::back:
			return "circle-left-solid.png";
		case fa_icon::floppy:
			return "floppy-disk-solid.png";
		case fa_icon::play:
			return "play-button-arrowhead.png";
		case fa_icon::poweroff:
			return "power-off-solid.png";
		case fa_icon::restart:
			return "rotate-left-solid.png";
		case fa_icon::screenshot:
			return "screenshot.png";
		case fa_icon::video_camera:
			return "video-camera.png";
		case fa_icon::friends:
			return "user-group-solid.png";
		case fa_icon::trophy:
			return "trophy-solid.png";
		case fa_icon::audio:
			return "headphones-solid.png";
		case fa_icon::video:
			return "display-solid.png";
		case fa_icon::gamepad:
			return "gamepad-solid.png";
		case fa_icon::settings_sliders:
			return "sliders-solid.png";
		case fa_icon::settings_gauge:
			return "gauge-solid.png";
		case fa_icon::bug:
			return "bug-solid.png";
		}
	}

	const image_info* get_icon(fa_icon icon)
	{
		if (icon == fa_icon::none || static_cast<usz>(icon) >= icon_count)
		{
			return nullptr;
		}

		retro_atomic_ptr_t& slot = g_icons_cache.slots[static_cast<usz>(icon)];

		if (const auto loaded = static_cast<const image_info*>(retro_atomic_load_acquire_ptr(&slot)))
		{
			return loaded;
		}

		const std::string image_path = fmt::format("home/32/%s", fa_icon_to_filename(icon));
		image_info* const mine = rsx::overlays::resource_config::load_icon(image_path).release();

		if (retro_atomic_cas_ptr(&slot, nullptr, mine))
		{
			return mine;
		}

		// Another loader got in first: its icon is the one kept
		delete mine;
		return static_cast<const image_info*>(retro_atomic_load_acquire_ptr(&slot));
	}
}
