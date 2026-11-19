#pragma once

// RPCS3's game patches for the emulator: the database and the patching are
// patch_db.c's; this is what only the emulator can do (guest memory, code
// caves, branches, breakpoints, file mounts) and where the files are.

#include "util/types.hpp"
#include "Utilities/patch_db.h"

#include <string>
#include <string_view>
#include <vector>

class patch_engine
{
public:
	patch_engine();
	~patch_engine();

	patch_engine(const patch_engine&) = delete;

	patch_engine& operator=(const patch_engine&) = delete;

	// Returns the directory in which patch_config.yml is located
	static std::string get_patch_config_path();

	// Returns the directory in which patches are located
	static std::string get_patches_path();

	// Returns the filepath for the imported_patch.yml
	static std::string get_imported_patch_path();

	// Reads a patch file, with patch_config.yml saying which patches are on,
	// into @db: true also when there is no such file
	static bool load(patch_db_t& db, const std::string& path);

	// The database's messages to the log (patch_db_init's log)
	static void log(void* user, int level, const char* msg, usz len);

	// Writes patch_config.yml with these patches on, and no other
	static void save_config(const patch_db_t& db, const patch_config_choice_t* choices, usz count);

	// Load from file and append to member patches map
	void append_global_patches();

	// Load from title relevant files and append to member patches map
	void append_title_patches(std::string_view title_id);

	// Applies the patches on for the executable @name: @translate(offset,
	// size) is its memory (u8*)
	template <typename F>
	void apply(std::vector<u32>& applied_total, const std::string& name, const F& translate, u32 filesz = -1, u32 min_addr = 0)
	{
		apply_with(applied_total, name, [](void* f, u32 addr, u32 size) -> u8* { return (*static_cast<const F*>(f))(addr, size); },
			const_cast<F*>(&translate), filesz, min_addr);
	}

	void apply_with(std::vector<u32>& applied_total, const std::string& name, u8* (*translate)(void*, u32, u32), void* translate_user, u32 filesz, u32 min_addr);

	// Deallocate memory used by patches
	void unload(const std::string& name);

private:
	patch_db_t m_db;
};
