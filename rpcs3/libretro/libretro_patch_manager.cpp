// The Patch Manager category (libretro_patch_options.c) and the files it
// lists: patch.yml (the one the core carries), imported_patch.yml (what was
// imported in standalone RPCS3, copied over) and the game's own
// <serial>_patch.yml, the three RPCS3 loads itself. Before each boot
// patch_config.yml is written from the options.

#include "stdafx.h"
#include "libretro_patch_manager.h"

#include "Utilities/bin_patch.h"
#include "Utilities/File.h"

LOG_CHANNEL(lr_patch_log, "libretro patches");

namespace
{
	patch_db_t s_db;
	bool s_db_ready = false;

	void log(void*, int level, const char* msg, usz len)
	{
		const std::string_view text(msg, len);

		switch (level & PATCH_LOG_LEVEL_MASK)
		{
		case PATCH_LOG_ERROR: lr_patch_log.error("%s", text); break;
		case PATCH_LOG_WARNING: lr_patch_log.warning("%s", text); break;
		default: lr_patch_log.notice("%s", text); break;
		}
	}

	void reset_db()
	{
		if (s_db_ready)
			patch_db_free(&s_db);
		else
			patch_db_init(&s_db, &patch_engine::log, nullptr);
		s_db_ready = true;
	}
}

namespace libretro_patches
{
	void collect(const std::string& serial)
	{
		reset_db();
		patch_options_set_log(log, nullptr);

		if (!serial.empty())
		{
			const std::string dir = patch_engine::get_patches_path();
			for (const std::string& path : {dir + "patch.yml", patch_engine::get_imported_patch_path(), dir + serial + "_patch.yml"})
			{
				if (fs::is_file(path))
					patch_engine::load(s_db, path);
			}
		}

		patch_options_collect(serial.empty() ? nullptr : &s_db, serial.c_str());
	}

	bool any()
	{
		return patch_options_any() != 0;
	}

	const retro_core_option_v2_definition* definitions(std::size_t& count)
	{
		return patch_options_definitions(&count);
	}

	void update_display(retro_environment_t cb, patch_options_get_t get, void* user)
	{
		patch_options_update_display(cb, get, user);
	}

	void write_config(patch_options_get_t get, void* user)
	{
		std::size_t count = 0;
		const patch_config_choice_t* const choices = patch_options_choices(get, user, &count);

		if (!s_db_ready)
			reset_db();

		patch_engine::save_config(s_db, choices, count);
	}
}
