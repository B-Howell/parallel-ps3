#pragma once

#include "util/types.hpp"

#include <retro_atomic.h>
#include <rthreads/retro_waitaddr.h>
#include "util/shared_ptr.hpp"
#include "Utilities/bit_set.h"
#include "config_mode.h"
#include "games_config.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <set>

void init_fxo_for_exec(utils::serial*, bool);

enum class video_renderer;

class spu_thread;

template <typename T>
class named_thread;

enum class system_state : u32
{
	stopped,
	loading,
	stopping,
	running,
	paused,
	frozen, // paused but cannot resume
	ready,
	starting,
};

enum class game_boot_result : u32
{
	no_errors,
	generic_error,
	nothing_to_boot,
	wrong_disc_location,
	invalid_file_or_folder,
	invalid_bdvd_folder,
	install_failed,
	decryption_error,
	file_creation_error,
	firmware_missing,
	firmware_version,
	unsupported_disc_type,
	disc_key_missing,
	disc_key_invalid,
	savestate_corrupted,
	savestate_version_unsupported,
	still_running,
	already_added,
	currently_restricted,
	database_config_missing,
};

constexpr bool is_error(game_boot_result res)
{
	return res != game_boot_result::no_errors;
}

namespace utils
{
	struct serial;
};

struct emu_precompilation_option_t
{
	bool is_fast = false;
};

// Bumped, and its waiters woken, at every change of the emulator's state or
// of a stop pending, and by every retro_run: a thread waiting for any of them
// waits on it, untimed, where standalone RPCS3 sleeps and looks again
extern retro_atomic_int_t g_libretro_events;
inline void libretro_event()
{
	retro_atomic_fetch_add_int(&g_libretro_events, 1);
	retro_waitaddr_notify_all(&g_libretro_events);
}

// Its count now, to wait for the next
inline u32 libretro_events_seen()
{
	return static_cast<u32>(retro_atomic_load_acquire_int(&g_libretro_events));
}

// Until it moves on from @seen (it may return early: the caller looks again)
inline void libretro_events_wait(u32 seen)
{
	const retro_waitaddr_t w{&g_libretro_events, seen};
	retro_waitaddr_wait(&w, 1, -1);
}

// The same on one of the core's named threads, also woken as the thread is
// told to end (System.cpp)
void libretro_events_wait_named(u32 seen);

// A host thread's wait for @word to leave @old, or for the emulator's state
// to change (a stop that skips what was waited for is one): untimed, no
// poll. The caller looks again at what it waits for, and at the state
inline void libretro_wait_on(retro_atomic_int_t& word, u32 old)
{
	const u32 events = libretro_events_seen();

	if (static_cast<u32>(retro_atomic_load_acquire_int(&word)) != old)
	{
		return;
	}

	const retro_waitaddr_t w[2] = {{&word, old}, {&g_libretro_events, events}};
	retro_waitaddr_wait(w, 2, -1);
}

class Emulator final
{
	// The emulator's state (system_state). WaitReady and wait_state_change
	// wait on it; every change wakes them
	mutable retro_atomic_int_t m_state{static_cast<int>(system_state::stopped)};

	system_state state() const { return static_cast<system_state>(retro_atomic_load_acquire_int(&m_state)); }
	void set_state(system_state to)
	{
		retro_atomic_store_release_int(&m_state, static_cast<int>(to));
		retro_waitaddr_notify_all(&m_state);
	}

	bool state_cas(system_state from, system_state to)
	{
		if (!retro_atomic_cas_int(&m_state, static_cast<int>(from), static_cast<int>(to)))
		{
			return false;
		}

		retro_waitaddr_notify_all(&m_state);
		return true;
	}

	retro_atomic_64_t m_pause_start_time{0}; // set when paused
	mutable retro_atomic_64_t m_pause_amend_time{0}; // increased when resumed
	mutable retro_atomic_64_t m_stop_ctr{1}; // Increments when emulation is stopped
	mutable retro_atomic_int_t m_emu_state_close_pending{0};
	mutable retro_atomic_64_t m_restrict_emu_state_change{0};

	u64 stop_ctr() const { return static_cast<u64>(retro_atomic_load_acquire_64(&m_stop_ctr)); }

	// The state or a stop pending changed
	void state_changed()
	{
		libretro_event();
	}

	games_config m_games_config;

	std::set<video_renderer> m_supported_renderers;
	video_renderer m_default_renderer;
	std::string m_default_graphics_adapter;

	cfg_mode m_config_mode = cfg_mode::custom;
	std::string m_config_path;
	std::optional<std::string> m_db_config; // std::nullopt means it has not been retrieved yet
	std::string m_path;
	std::string m_path_old;
	std::string m_path_original;
	std::string m_path_real;
	std::string m_title_id;
	std::string m_title;
	std::string m_localized_title;
	std::string m_app_version;
	std::string m_hash;
	std::string m_cat;
	std::string m_dir;
	std::string m_sfo_dir;
	std::string m_game_dir{"PS3_GAME"};
	std::string m_usr{"00000001"};
	u32 m_usrid{1};
	std::shared_ptr<utils::serial> m_ar;

	// This flag should be adjusted before each Kill() or each BootGame() and similar because:
	// 1. It forces an application to boot immediately by calling Run() in Load().
	// 2. It signifies that we don't want to exit on Kill(), for example if we want to transition to another application.
	bool m_force_boot = false;

	bool m_continuous_mode = false;
	bool m_has_gui = true;
	bool m_headless = false;
	bool m_add_database_config = false;

	bool m_state_inspection_savestate = false;


	std::vector<std::shared_ptr<retro_atomic_int_t>> m_pause_msgs_refs;

	std::vector<std::function<void()>> m_postponed_init_code;

	void ExecPostponedInitCode()
	{
		for (auto&& func : ::as_rvalue(std::move(m_postponed_init_code)))
		{
			func();
		}
	}

	enum class SaveStateExtentionFlags1 : u8
	{
		SupportsMenuOpenResume,
		ShouldCloseMenu,

		__bitset_enum_max,
	};

	bs_t<SaveStateExtentionFlags1> m_savestate_extension_flags1{};
	emu_precompilation_option_t m_precompilation_option{};

public:
	static constexpr std::string_view game_id_boot_prefix = "%RPCS3_GAMEID%:";
	static constexpr std::string_view vfs_boot_prefix = "%RPCS3_VFS%:";

	Emulator() noexcept;
	~Emulator() noexcept;

	static bool IsAvailable() noexcept;

	void SetGameDir(const std::string& game_dir) { m_game_dir = game_dir; }

	// Call from the GUI thread
	void CallFromMainThread(std::function<void()>&& func, retro_atomic_int_t* wake_up = nullptr, bool track_emu_state = true, u64 stop_ctr = umax,
		std::source_location src_loc = std::source_location::current()) const;

	// Blocking call from the GUI thread
	void BlockingCallFromMainThread(std::function<void()>&& func, bool track_emu_state = true, std::source_location src_loc = std::source_location::current()) const;

	enum class stop_counter_t : u64{};

	// Returns a different value each time we start a new emulation.
	stop_counter_t GetEmulationIdentifier(bool subtract_one = false) const
	{
		if (subtract_one)
		{
			return stop_counter_t{stop_ctr() - 1};
		}

		return stop_counter_t{stop_ctr()};
	}

	void CallFromMainThread(std::function<void()>&& func, stop_counter_t counter,
		std::source_location src_loc = std::source_location::current()) const
	{
		CallFromMainThread(std::move(func), nullptr, true, static_cast<u64>(counter), src_loc);
	}

	void PostponeInitCode(std::function<void()>&& func)
	{
		m_postponed_init_code.emplace_back(std::move(func));
	}

	/** Set emulator mode to running unconditionnaly.
	 * Required to execute various part (PPUInterpreter, memory manager...) outside of rpcs3.
	 */
	void SetTestMode()
	{
		set_state(system_state::running);
		state_changed();
	}

	void SetPrecompileCacheOption(emu_precompilation_option_t option)
	{
		m_precompilation_option = option;
	}

	void Init();

	std::vector<std::string> argv;
	std::vector<std::string> envp;
	std::vector<u8> data;
	std::vector<u128> klic;
	std::string disc;
	std::string hdd1;
	std::function<void(u32)> init_mem_containers;
	std::function<void()> after_kill_callback;

	u32 m_boot_source_type = 0; // CELL_GAME_GAMETYPE_SYS

	u32 GetBootSourceType() const
	{
		return m_boot_source_type;
	}

	const std::string& GetBoot() const
	{
		return m_path;
	}

	const std::string& GetLastBoot() const
	{
		return m_path_original;
	}

	const std::string& GetTitleID() const
	{
		return m_title_id;
	}

	const std::string& GetTitle() const
	{
		return m_title;
	}

	const std::string& GetLocalizedTitle() const
	{
		return m_localized_title;
	}

	const std::string GetTitleAndTitleID() const
	{
		return m_title + (m_title_id.empty() ? "" : " [" + m_title_id + "]");
	}

	const std::string& GetAppVersion() const
	{
		return m_app_version;
	}

	const std::string& GetExecutableHash() const
	{
		return m_hash;
	}

	void SetExecutableHash(std::string hash) { m_hash = std::move(hash); }

	const std::string& GetCat() const
	{
		return m_cat;
	}

	const std::string& GetFakeCat() const;

	const std::string& GetDir() const
	{
		return m_dir;
	}

	const std::string GetSfoDir(bool prefer_disc_sfo) const;

	// String for GUI dialogs.
	const std::string& GetUsr() const
	{
		return m_usr;
	}

	const games_config& GetGamesConfig() const
	{
		return m_games_config;
	}

	// Get deserialization manager
	utils::serial* DeserialManager() const;

	// u32 for cell.
	u32 GetUsrId() const
	{
		return m_usrid;
	}

	void SetUsr(const std::string& user);

	u64 GetPauseTime() const
	{
		return static_cast<u64>(retro_atomic_load_acquire_64(&m_pause_amend_time));
	}

	const std::string& GetUsedConfig() const
	{
		return m_config_path;
	}

	const std::string& GetUsedDatabaseConfig() const
	{
		static std::string empty_db_config;
		return m_db_config ? *m_db_config : empty_db_config;
	}

	bool IsChildProcess() const
	{
		return m_config_mode == cfg_mode::continuous;
	}

	bool ContinuousModeEnabled(bool reset)
	{
		if (reset)
		{
			return std::exchange(m_continuous_mode, false);
		}
		return m_continuous_mode;
	}

	class emulation_state_guard_t
	{
		class Emulator* _this = nullptr;
		bool active = true;

	public:
		explicit emulation_state_guard_t(Emulator* this0) noexcept
			: _this(this0)
		{
			retro_atomic_fetch_add_64(&_this->m_restrict_emu_state_change, 1);
		}

		~emulation_state_guard_t() noexcept
		{
			if (active)
			{
				// Down by one unless already 0
				for (s64 n = retro_atomic_load_acquire_64(&_this->m_restrict_emu_state_change); n > 0 && !retro_atomic_cas_64(&_this->m_restrict_emu_state_change, n, n - 1);)
				{
					n = retro_atomic_load_acquire_64(&_this->m_restrict_emu_state_change);
				}
			}
		}

		emulation_state_guard_t(emulation_state_guard_t&& rhs) noexcept
		{
			_this = rhs._this;
			active = std::exchange(rhs.active, false);
		}

		emulation_state_guard_t& operator=(const emulation_state_guard_t&) = delete;
		emulation_state_guard_t(const emulation_state_guard_t&) = delete;
	};

	emulation_state_guard_t MakeEmulationStateGuard()
	{
		return emulation_state_guard_t{this};
	}

	game_boot_result BootGame(const std::string& path, const std::string& title_id = "", bool direct = false, cfg_mode config_mode = cfg_mode::custom, const std::string& config_path = "", const std::optional<std::string>& db_config = std::nullopt);

	// Boots a minimal shell (no PS3 executable) that hosts the RSX-overlay-based Big Picture Mode game grid.
	// Cancel any pending return to Big Picture Mode, e.g. when a game is booted manually and bypasses the shell.

	void SetForceBoot(bool force_boot);
	void SetContinuousMode(bool continuous_mode);

	game_boot_result Load(const std::string& title_id = "", bool is_disc_patch = false, usz recursion_count = 0);
	void Run(bool start_playtime);
	void RunPPU();
	void FixGuestTime();
	void FinalizeRunRequest();

	bool IsBootingRestricted() const
	{
		return retro_atomic_load_acquire_64(&m_restrict_emu_state_change) != 0;
	}

private:
	struct savestate_stage
	{
		bool prepared = false;
		std::vector<std::pair<stx::shared_ptr<named_thread<spu_thread>>, u32>> paused_spus;
	};
public:

	bool Pause(bool freeze_emulation = false, bool show_resume_message = true);
	void Resume();
	void GracefulShutdown(bool allow_autoexit = true, bool async_op = false, bool savestate = false, bool continuous_mode = false);
	void Kill(bool allow_autoexit = true, bool savestate = false, savestate_stage* stage = nullptr);
	game_boot_result Restart(bool graceful = true, bool reset_path = true);
	bool Quit(bool force_quit);
	static void CleanUp();

	bool IsRunning() const { return state() == system_state::running; }
	bool IsPaused() const { const system_state s = state(); return s >= system_state::paused && s <= system_state::frozen; }
	bool IsPausedOrReady() const { return state() >= system_state::paused; }
	bool IsStopped(bool test_fully = false) const { return test_fully ? state() == system_state::stopped : state() <= system_state::stopping; }
	bool IsReady()   const { return state() == system_state::ready; }
	bool IsStarting() const { return state() == system_state::starting; }
	// A stop (or a savestate's preparation for one) is under way
	bool IsClosePending() const { return retro_atomic_load_acquire_int(&m_emu_state_close_pending) != 0; }

	// Until the state leaves ready (a wake may be spurious: the caller looks again)
	void WaitReady() const
	{
		wait_state_change(system_state::ready);
	}

	// Until the state is no longer @old (a wake may be spurious: the caller looks again)
	void wait_state_change(system_state old) const
	{
		const retro_waitaddr_t w{&m_state, static_cast<u32>(old)};
		retro_waitaddr_wait(&w, 1, -1);
	}

	// A wait list entry for the state, seen as @old
	retro_waitaddr_t state_waitaddr(system_state old) const
	{
		return {&m_state, static_cast<u32>(old)};
	}
	auto GetStatus(bool fixup = true) const { const system_state s = state(); return fixup && s == system_state::frozen ? system_state::paused : fixup && s == system_state::stopping ? system_state::stopped : s; }

	bool HasGui() const { return m_has_gui; }
	void SetHasGui(bool has_gui) { m_has_gui = has_gui; }

	bool IsHeadless() const { return m_headless; }
	void SetHeadless(bool headless) { m_headless = headless; }

	const std::set<video_renderer>& GetSupportedRenderers() const { return m_supported_renderers; }
	void SetSupportedRenderers(std::set<video_renderer> renderers) { m_supported_renderers = std::move(renderers); }

	video_renderer GetDefaultRenderer() const { return m_default_renderer; }
	void SetDefaultRenderer(video_renderer renderer) { m_default_renderer = renderer; }
	void SetDefaultGraphicsAdapter(std::string adapter) { m_default_graphics_adapter = std::move(adapter); }

	std::string GetFormattedTitle(double fps) const;

	void ConfigurePPUCache() const;

	std::set<std::string> GetGameDirs() const;

	// Check if path is inside the specified directory
	bool IsPathInsideDir(std::string_view path, std::string_view dir, bool check_if_exists = true) const;
	game_boot_result VerifyPathCasing(std::string_view path, std::string_view dir, bool from_dir) const;

	void EjectDisc();
	game_boot_result InsertDisc(const std::string& path);

	static game_boot_result GetElfPathFromDir(std::string& elf_path, const std::string& path);
	static void GetBdvdDir(std::string& bdvd_dir, std::string& sfb_dir, std::string& game_dir, const std::string& elf_dir);
	friend void init_fxo_for_exec(utils::serial*, bool);

	static bool IsVsh();
	static bool IsValidSfb(const std::string& path);

	static void SaveSettings(std::string_view settings, const std::string& title_id);
};

extern Emulator Emu;

// One of retro_run's frames, or a change of the emulator's state - and on
// one of the core's named threads, its being told to end (a join at close
// wakes it through the thread's own word: retro_run has stopped by then, so
// a wait for a frame alone never ends). False when the emulator stopped or
// this thread is told to end (System.cpp)
bool libretro_wait_frame();

// A host thread's wait for @frames of the game's time: retro_run's frames
// (g_libretro_events), never the host's clock. False when the emulator
// stopped first, or the thread is told to end. Where standalone RPCS3
// sleeps some milliseconds to let the game get on, this lets it get on that
// many frames
inline bool libretro_wait_frames(u32 frames)
{
	for (u32 n = 0; n < frames; n++)
	{
		if (!libretro_wait_frame())
		{
			return false;
		}
	}

	return !Emu.IsStopped();
}
