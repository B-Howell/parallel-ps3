#pragma once

#include <retro_atomic.h>
#include "util/shared_ptr.hpp"

#include <functional>

enum class localized_string_id;

namespace cfg
{
	class _base;
}

// Plain functions: each the core sets captures nothing (it was a std::function apiece)
struct emu_callbacks
{
	void (*call_from_main_thread)(std::function<void()>, retro_atomic_int_t*) = nullptr; // the word set to 1, and woken, once it ran
	void (*on_run)(bool) = nullptr; // (start_playtime) continuing or going ingame, so start the clock
	void (*on_pause)() = nullptr;
	void (*on_resume)() = nullptr;
	void (*on_stop)() = nullptr;
	void (*on_ready)() = nullptr;
	void (*on_missing_fw)() = nullptr;
	void (*enable_disc_eject)(bool enabled) = nullptr;
	void (*enable_disc_insert)(bool enabled) = nullptr;
	bool (*try_to_quit)(bool, std::function<void()>) = nullptr; // (force_quit, on_exit) Try to close RPCS3
	void (*handle_taskbar_progress)(s32, s32) = nullptr; // (type, value) type: 0 for reset, 1 for increment, 2 for set_limit, 3 for set_value
	void (*init_kb_handler)() = nullptr;
	void (*init_mouse_handler)() = nullptr;
	void (*init_pad_handler)(std::string_view title_id) = nullptr;
	void (*update_emu_settings)() = nullptr;
	void (*save_emu_settings)() = nullptr;
	void (*close_gs_frame)() = nullptr;
	std::unique_ptr<class GSFrameBase> (*get_gs_frame)() = nullptr;
	std::shared_ptr<class camera_handler_base> (*get_camera_handler)() = nullptr;
	std::shared_ptr<class music_handler_base> (*get_music_handler)() = nullptr;
	void (*init_gs_render)(utils::serial*) = nullptr;
	std::shared_ptr<class AudioBackend> (*get_audio)() = nullptr;
	std::shared_ptr<class audio_device_enumerator> (*get_audio_enumerator)(u64) = nullptr; // (audio_renderer)
	std::shared_ptr<class MsgDialogBase> (*get_msg_dialog)() = nullptr;
	std::shared_ptr<class OskDialogBase> (*get_osk_dialog)() = nullptr;
	std::unique_ptr<class SaveDialogBase> (*get_save_dialog)() = nullptr;
	std::shared_ptr<class SendMessageDialogBase> (*get_sendmessage_dialog)() = nullptr;
	std::shared_ptr<class RecvMessageDialogBase> (*get_recvmessage_dialog)() = nullptr;
	std::unique_ptr<class TrophyNotificationBase> (*get_trophy_notification_dialog)() = nullptr;
	std::string (*get_localized_string)(localized_string_id, const char*) = nullptr;
	std::u32string (*get_localized_u32string)(localized_string_id, const char*) = nullptr;
	std::string (*get_localized_setting)(const cfg::_base*, u32) = nullptr;
	std::string (*get_photo_path)(std::string_view) = nullptr;
	void (*play_sound)(const std::string&, std::optional<f32>) = nullptr;
	bool (*get_image_info)(const std::string&, std::string&, s32&, s32&, s32&) = nullptr; // (filename, sub_type, width, height, CellSearchOrientation)
	bool (*get_scaled_image)(const std::string&, s32, s32, s32&, s32&, u8*, bool) = nullptr; // (filename, target_width, target_height, width, height, dst, force_fit)
	std::string (*resolve_path)(std::string_view) = [](std::string_view arg){ return std::string{arg}; }; // Resolve path using Qt (returns empty string if the file doesn't exist)
	std::string (*resolve_path_may_not_exist)(std::string_view) = [](std::string_view arg){ return std::string{arg}; }; // Resolve path using Qt
	std::vector<std::string> (*get_font_dirs)() = nullptr;
	bool (*on_install_pkgs)(const std::vector<std::string>&, bool) = nullptr;
	void (*add_breakpoint)(u32) = nullptr;
	bool (*display_sleep_control_supported)() = nullptr;
	void (*enable_display_sleep)(bool) = nullptr;
	void (*check_microphone_permissions)() = nullptr;
	std::unique_ptr<class video_source> (*make_video_source)() = nullptr;
	void (*enable_gamemode)(bool) = nullptr;
	std::string (*get_database_config)(const std::string&) = nullptr;
};

extern emu_callbacks g_emu_callbacks;
