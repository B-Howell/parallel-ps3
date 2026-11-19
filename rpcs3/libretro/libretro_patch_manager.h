#pragma once

// RPCS3's game patches as core options: the Patch Manager category. The
// options are libretro_patch_options.c's; this reads the patch files for
// them and writes patch_config.yml.

#include "libretro.h"
#include "libretro_patch_options.h"

#include <cstddef>
#include <string>

namespace libretro_patches
{
	// The patches for one game: what patch.yml, imported_patch.yml and the
	// game's own <serial>_patch.yml have for its serial. Empty clears them.
	void collect(const std::string& serial);

	// Whether the collected game has any patches (and so the category)
	bool any();

	// The options for the collected patches, @count of them, to be declared
	// after the fixed ones. They stay valid until the next collect().
	const retro_core_option_v2_definition* definitions(std::size_t& count);

	// Shows the value choices of a patch only while the patch is on
	void update_display(retro_environment_t cb, patch_options_get_t get, void* user);

	// Writes patch_config.yml for what the options turn on. RPCS3 reads it when
	// the game boots, so a change applies at the next start.
	void write_config(patch_options_get_t get, void* user);
}
