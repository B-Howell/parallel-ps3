#include "stdafx.h"
#include "system_progress.hpp"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/Overlays/overlay_manager.h"
#include "Emu/RSX/Overlays/overlay_message_dialog.h"
#include "Emu/RSX/Overlays/overlay_message.h"
#include "Emu/RSX/Overlays/overlay_compile_notification.h"
#include "Emu/emu_callbacks.h"
#include "Emu/System.h"
#include "Emu/system_config.h"

#include "util/asm.hpp"

LOG_CHANNEL(sys_log, "SYS");

// Progress display server synchronization variables
lf_array<atomic_ptr<std::string>> g_progr_text_queue;
progress_dialog_string_t g_progr_text{};
retro_atomic_int_t g_progr_ftotal{0};
retro_atomic_int_t g_progr_fdone{0};
retro_atomic_64_t g_progr_ftotal_bits{0};
retro_atomic_64_t g_progr_fknown_bits{0};
retro_atomic_int_t g_progr_ptotal{0};
retro_atomic_int_t g_progr_pdone{0};

// For Batch PPU Compilation
retro_atomic_int_t g_system_progress_canceled{0};

// For showing feedback while stopping emulation
retro_atomic_int_t g_system_progress_stopping{system_progress_stop_state::stop_state_disabled};

namespace rsx::overlays
{
	class progress_dialog : public message_dialog
	{
	public:
		using message_dialog::message_dialog;
	};
} // namespace rsx::overlays

void progress_dialog_server::operator()()
{
	std::shared_ptr<rsx::overlays::progress_dialog> native_dlg;
	retro_atomic_store_release_int(&g_system_progress_stopping, system_progress_stop_state::stop_state_disabled);
	retro_atomic_store_release_int(&g_system_progress_canceled, 0);

	const auto get_state = []()
	{
		auto whole_state = std::make_tuple(g_progr_text.operator std::string(), progr_load(g_progr_ftotal), progr_load(g_progr_fdone), progr_load(g_progr_ftotal_bits), progr_load(g_progr_fknown_bits), progr_load(g_progr_ptotal), progr_load(g_progr_pdone));

		while (true)
		{
			auto new_state = std::make_tuple(g_progr_text.operator std::string(), progr_load(g_progr_ftotal), progr_load(g_progr_fdone), progr_load(g_progr_ftotal_bits), progr_load(g_progr_fknown_bits), progr_load(g_progr_ptotal), progr_load(g_progr_pdone));

			if (new_state == whole_state)
			{
				// Only leave while it has a complete (atomic) state
				return whole_state;
			}

			whole_state = std::move(new_state);
		}

		return whole_state;
	};

	// Until the next of retro_run's frames, a change of the emulator's state
	// (a stop among them), or this thread being told to end: a close joins
	// it after retro_run has stopped, so a frame alone would never come
	// (this is a named thread, so its own word is in the wait). Not
	// libretro_wait_frame: that returns at once once the emulator stops, and
	// from a close's change of state to its join this would spin on it
	const auto next_frame = []()
	{
		const u32 events = libretro_events_seen();
		libretro_events_wait_named(events);
	};

	const auto create_native_dialog = [&native_dlg](const std::string& text, bool* show_overlay_message)
	{
		if (const auto renderer = rsx::get_current_renderer())
		{
			// Some backends like OpenGL actually initialize a lot of driver objects in the "on_init" method.
			// Wait for init to complete within 300 of retro_run's frames (5 seconds of the game's
			// time), as it is set or a frame passes, never the host's clock. Abort just in case
			// we have hardware/driver issues.
			for (u32 frames = 0; frames < 300 && !static_cast<u32>(retro_atomic_load_acquire_int(&renderer->is_initialized)) && !Emu.IsStopped() && thread_ctrl::state() != thread_state::aborting; frames++)
			{
				const u32 events = libretro_events_seen();

				if (static_cast<u32>(retro_atomic_load_acquire_int(&renderer->is_initialized)))
				{
					break;
				}

				thread_ctrl::wait_on_custom<2>([&](atomic_wait::list<4>& list)
				{
					list.template set_raw<0>(&renderer->is_initialized, 0u);
					list.template set_raw<1>(&g_libretro_events, events);
				});
			}

			auto manager = g_fxo->try_get<rsx::overlays::display_manager>();

			if (show_overlay_message)
			{
				*show_overlay_message = retro_atomic_load_acquire_int(&g_fxo->get<progress_dialog_workaround>().show_overlay_message_only) != 0;
				if (*show_overlay_message)
				{
					return;
				}
			}

			if (manager)
			{
				MsgDialogType type{};
				type.se_mute_on         = true;
				type.se_normal          = true;
				type.bg_invisible       = true;
				type.disable_cancel     = true;
				type.progress_bar_count = 1;

				native_dlg = manager->create<rsx::overlays::progress_dialog>(true);
				native_dlg->show(false, text, type, msg_dialog_source::sys_progress, nullptr);
				native_dlg->progress_bar_set_message(0, get_localized_string(localized_string_id::PROGRESS_DIALOG_PLEASE_WAIT));
			}
		}
	};

	while (!progr_stopping() && thread_ctrl::state() != thread_state::aborting)
	{
		// Wait for the start condition
		std::string text0 = g_progr_text;

		while (text0.empty())
		{
			if (progr_stopping() || thread_ctrl::state() == thread_state::aborting)
			{
				break;
			}

			if (progr_load(g_progr_ftotal) || progr_load(g_progr_fdone) || progr_load(g_progr_ptotal) || progr_load(g_progr_pdone))
			{
				const auto [text_new, ftotal, fdone, ftotal_bits, fknown_bits, ptotal, pdone] = get_state();

				if (!text_new.empty())
				{
					text0 = text_new;
					break;
				}

				if ((ftotal || ptotal) && ftotal == fdone && ptotal == pdone)
				{
					// Cleanup (missed message but do not cry over spilt milk)
					progr_sub(g_progr_fdone, fdone);
					progr_sub(g_progr_pdone, pdone);
					progr_sub(g_progr_ftotal_bits, ftotal_bits);
					progr_sub(g_progr_fknown_bits, fknown_bits);
					progr_sub(g_progr_ftotal, ftotal);
					progr_sub(g_progr_ptotal, ptotal);
					retro_waitaddr_notify_all(&g_progr_ptotal);
				}
			}

			// A look a frame: retro_run's (g_libretro_events), no clock
			next_frame();
			text0 = g_progr_text;
		}

		if (progr_stopping() || thread_ctrl::state() == thread_state::aborting)
		{
			break;
		}

		retro_atomic_store_release_int(&g_system_progress_canceled, 0);

		// Initialize message dialog
		bool show_overlay_message = false; // Only show an overlay message after initial loading is done.
		std::shared_ptr<MsgDialogBase> dlg;

		create_native_dialog(text0, &show_overlay_message);

		if (!show_overlay_message && !native_dlg && (dlg = g_emu_callbacks.get_msg_dialog()))
		{
			dlg->type.se_normal          = true;
			dlg->type.bg_invisible       = true;
			dlg->type.progress_bar_count = 1;
			dlg->on_close = [](s32 /*status*/)
			{
				Emu.CallFromMainThread([]()
				{
					// Abort everything
					sys_log.notice("Aborted progress dialog");
					Emu.GracefulShutdown(false, true);
				});

				retro_atomic_store_release_int(&g_system_progress_canceled, 1);
			};

			Emu.CallFromMainThread([dlg, text0]()
			{
				dlg->Create(text0, text0);
			});
		}

		u32 ftotal = 0;
		u32 fdone = 0;
		u64 fknown_bits = 0;
		u64 ftotal_bits = 0;
		u32 ptotal = 0;
		u32 pdone = 0;
		std::string text1;

		const u64 start_time = get_system_time();
		u64 wait_no_update_count = 0;

		std::shared_ptr<retro_atomic_int_t> ppu_cue_refs;

		std::vector<std::pair<u64, u64>> time_left_queue(1024);
		usz time_left_queue_idx = 0;

		// Update progress
		// An update a frame, as the dialog is drawn: retro_run's, no clock
		for (;
			!progr_stopping() && thread_ctrl::state() != thread_state::aborting;
			next_frame())
		{
			const auto [text_new, ftotal_new, fdone_new, ftotal_bits_new, fknown_bits_new, ptotal_new, pdone_new] = get_state();

			// Force-update every 20 seconds (at 60 frames) to update remaining time
			if (wait_no_update_count == 60u * 20 || ftotal != ftotal_new || fdone != fdone_new || fknown_bits != fknown_bits_new
				|| ftotal_bits != ftotal_bits_new || ptotal != ptotal_new || pdone != pdone_new || text_new != text1)
			{
				wait_no_update_count = 0;
				ftotal = ftotal_new;
				fdone  = fdone_new;
				ftotal_bits = ftotal_bits_new;
				fknown_bits = fknown_bits_new;
				ptotal = ptotal_new;
				pdone  = pdone_new;

				const bool text_changed = !text_new.empty() && text_new != text1;

				if (!text_new.empty())
				{
					text1 = text_new;
				}

				if (text1.empty())
				{
					// Cannot do anything
					continue;
				}

				if (show_overlay_message)
				{
					// Show a message instead (if compilation period is estimated to be lengthy)
					if (pdone < ptotal && g_cfg.misc.show_ppu_compilation_hint)
					{
						const u64 passed_usec = (get_system_time() - start_time);
						const u64 remaining_usec = pdone ? utils::rational_mul<u64>(passed_usec, static_cast<u64>(ptotal) - pdone, pdone) : (passed_usec * ptotal);

						// Only show compile notification if we estimate at least 100ms
						if (remaining_usec >= 100'000ULL)
						{
							if (!ppu_cue_refs || !retro_atomic_load_acquire_int(ppu_cue_refs.get()))
							{
								ppu_cue_refs = rsx::overlays::show_ppu_compile_notification();
							}

							// Make sure to update any pending messages. PPU compilation may freeze the image.
							rsx::overlays::refresh_message_queue();
						}
					}

					if (pdone >= ptotal)
					{
						if (ppu_cue_refs)
						{
							retro_atomic_store_release_int(ppu_cue_refs.get(), 0);
							ppu_cue_refs.reset();

							rsx::overlays::refresh_message_queue();
						}
					}

					continue;
				}

				// Compute new progress in percents
				// Assume not all programs were found if files were not compiled (as it may contain more)
				const bool use_bits = fknown_bits && ftotal_bits;
				const u64 known_files = use_bits ? fknown_bits : ftotal;
				const u64 total = utils::rational_mul<u64>(std::max<u64>(ptotal, 1), std::max<u64>(use_bits ? ftotal_bits : ftotal, 1), std::max<u64>(known_files, 1));
				const u64 done  = pdone;
				const u32 value = static_cast<u32>(done >= total ? 100 : done * 100 / total);

				std::string progr;

				if (ftotal || ptotal)
				{
					progr = get_localized_string(localized_string_id::PROGRESS_DIALOG_PROGRESS);

					if (ftotal)
						fmt::append(progr, " %s %u %s %u%s", get_localized_string(localized_string_id::PROGRESS_DIALOG_FILE), fdone, get_localized_string(localized_string_id::PROGRESS_DIALOG_OF), ftotal, ptotal ? "," : "");
					if (ptotal)
						fmt::append(progr, " %s %u %s %u", get_localized_string(localized_string_id::PROGRESS_DIALOG_MODULE), pdone, get_localized_string(localized_string_id::PROGRESS_DIALOG_OF), ptotal);

					const u32 of_1000 = static_cast<u32>(done >= total ? 1000 : done * 1000 / total);

					if (of_1000 >= 2)
					{
						const u64 passed = (get_system_time() - start_time);
						const u64 total = utils::rational_mul<u64>(passed, 1000, of_1000);
						const u64 remaining = total - passed;

						// Stabilize the result by using the maximum one from the recent history
						// This is a very simple approach yet appears to solve most inconsistencies
						u64 max_remaining = remaining;

						for (usz i = 0; i < time_left_queue.size(); i++)
						{
							const auto& sample = time_left_queue[(time_left_queue.size() + time_left_queue_idx - i) % time_left_queue.size()];

							const u64 sample_age = passed - sample.first;

							if (passed - sample.first >= 4'000'000)
							{
								// Ignore old samples
								break;
							}

							max_remaining = std::max<u64>(max_remaining, sample.second >= sample_age ? sample.second - sample_age : 0);
						}

						if (auto new_val = std::make_pair(passed, remaining); time_left_queue[time_left_queue_idx] != new_val)
						{
							time_left_queue_idx = (time_left_queue_idx + 1) % time_left_queue.size();
							time_left_queue[time_left_queue_idx] = new_val;
						}

						const u64 max_seconds_remaining = max_remaining / 1'000'000;
						const u64 seconds = max_seconds_remaining % 60;
						const u64 minutes = (max_seconds_remaining / 60) % 60;
						const u64 hours = (max_seconds_remaining / 3600);

						if (passed < 4'000'000)
						{
							// Cannot rely on such small duration of time for estimation
						}
						else if (done >= total)
						{
							fmt::append(progr, " (%s)", get_localized_string(localized_string_id::PROGRESS_DIALOG_DONE));
						}
						else if (hours)
						{
							fmt::append(progr, " (%uh %2um %s)", hours, minutes, get_localized_string(localized_string_id::PROGRESS_DIALOG_REMAINING));
						}
						else if (minutes >= 2)
						{
							fmt::append(progr, " (%um %s)", minutes, get_localized_string(localized_string_id::PROGRESS_DIALOG_REMAINING));
						}
						else if (minutes == 0)
						{
							fmt::append(progr, " (%us %s)", std::max<u64>(seconds, 1), get_localized_string(localized_string_id::PROGRESS_DIALOG_REMAINING));
						}
						else
						{
							fmt::append(progr, " (%um %2us %s)", minutes, seconds, get_localized_string(localized_string_id::PROGRESS_DIALOG_REMAINING));
						}
					}
				}
				else
				{
					progr = get_localized_string(localized_string_id::PROGRESS_DIALOG_PROGRESS_ANALYZING);
				}

				// Changes detected, send update
				if (native_dlg)
				{
					if (text_changed)
					{
						native_dlg->set_text(text1);
					}

					native_dlg->progress_bar_set_message(0, std::move(progr));
					native_dlg->progress_bar_set_value(0, static_cast<f32>(value));
				}
				else if (dlg)
				{
					Emu.CallFromMainThread([=]()
					{
						if (text_changed)
						{
							dlg->SetMsg(text1);
						}

						dlg->ProgressBarSetMsg(0, progr);
						dlg->ProgressBarSetValue(0, value);
					});
				}
			}

			if (show_overlay_message)
			{
				// Make sure to update any pending messages. PPU compilation may freeze the image.
				rsx::overlays::refresh_message_queue();
			}

			// Leave only if total count is equal to done count
			if (ftotal == fdone && ptotal == pdone && text_new.empty())
			{
				// Complete state, empty message: close dialog
				break;
			}

			wait_no_update_count++;
		}

		if (ppu_cue_refs)
		{
			retro_atomic_store_release_int(ppu_cue_refs.get(), 0);
		}

		if (progr_stopping() || thread_ctrl::state() == thread_state::aborting)
		{
			break;
		}

		if (show_overlay_message)
		{
			// Do nothing
		}
		else if (native_dlg)
		{
			native_dlg->close(false, false);
			native_dlg.reset();
		}
		else if (dlg)
		{
			Emu.CallFromMainThread([=]()
			{
				dlg->Close(true);
			});
		}

		// Cleanup
		progr_sub(g_progr_fdone, fdone);
		progr_sub(g_progr_pdone, pdone);
		progr_sub(g_progr_ftotal_bits, ftotal_bits);
		progr_sub(g_progr_fknown_bits, fknown_bits);
		progr_sub(g_progr_ftotal, ftotal);
		progr_sub(g_progr_ptotal, ptotal);
		retro_waitaddr_notify_all(&g_progr_ptotal);
	}

	if (progr_stopping())
	{
		const std::string text = get_localized_string(
			progr_stopping() == system_progress_stop_state::stop_state_continuous_savestate
				? localized_string_id::PROGRESS_DIALOG_SAVESTATE_PLEASE_WAIT
				: localized_string_id::PROGRESS_DIALOG_STOPPING_PLEASE_WAIT
		);
		if (native_dlg)
		{
			native_dlg->set_text(text);
		}
		else
		{
			create_native_dialog(text, nullptr);
		}
		if (native_dlg)
		{
			native_dlg->refresh();
		}
	}

	if (retro_atomic_exchange_int(&g_progr_ptotal, 0))
	{
		retro_waitaddr_notify_all(&g_progr_ptotal);
	}
}

progress_dialog_server::~progress_dialog_server()
{
	retro_atomic_store_release_int(&g_progr_ftotal, 0);
	retro_atomic_store_release_int(&g_progr_fdone, 0);
	retro_atomic_store_release_64(&g_progr_ftotal_bits, 0);
	retro_atomic_store_release_64(&g_progr_fknown_bits, 0);
	retro_atomic_store_release_int(&g_progr_ptotal, 0);
	retro_atomic_store_release_int(&g_progr_pdone, 0);
	g_progr_text.data.release(std::common_type_t<progress_dialog_string_t::data_t>{});
}
