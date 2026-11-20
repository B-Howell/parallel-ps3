#pragma once

#include "util/serialization_ext.hpp"

#include <retro_atomic.h>

struct version_entry
{
	u16 type;
	u16 version;

	ENABLE_BITWISE_SERIALIZATION;
};

struct savestate_header
{
	ENABLE_BITWISE_SERIALIZATION;

	nse_t<u64, 1> magic;
	bool LE_format;
	bool state_inspection_support;
	nse_t<u64, 1> offset;
	b8 flag_versions_is_following_data;
};

struct hle_locks_t
{
	// A 32-bit word, waited on as one (retro_waitaddr): the HLE calls in it
	// while 0 or more, a finalize being evaluated (-1) or done (-2)
	retro_atomic_int_t lock_val{0};

	enum states : s32
	{
		waiting_for_evaluation = -1,
		finalized = -2,
	};

	[[noreturn]] void lock();
	bool try_lock();
	void unlock();
	bool try_finalize(std::function<bool()> test);
};

// A savestate kept in memory instead of a file, for the frontend's save
// states: while set, a savestate the emulator makes is written there (and
// booting memory_savestate_path reads it from there, where it lies). The
// libretro core owns the buffer; it is not touched while a boot from it is
// still reading it.
extern std::vector<u8>* g_memory_savestate;
inline const std::string memory_savestate_path = "/libretro/memory.SAVESTAT.zst";

std::shared_ptr<utils::serial> make_savestate_reader(const std::string& path);
bool load_and_check_reserved(utils::serial& ar, usz size);
bool is_savestate_version_compatible(const std::vector<version_entry>& data, bool is_boot_check);
std::vector<version_entry> get_savestate_versioning_data(fs::file&& file, std::string_view filepath);
bool is_savestate_compatible(fs::file&& file, std::string_view filepath);
bool is_savestate_compatible(const std::string& filepath);
std::vector<version_entry> read_used_savestate_versions();
std::string get_savestate_file(std::string_view title_id, std::string_view boot_path, s64 rel_id, u64 aggregate_file_size = umax);
bool boot_current_game_savestate(bool testing, u32 index);
void clean_savestates(std::string_view title_id, std::string_view boot_path, usz max_files, usz max_files_size);
