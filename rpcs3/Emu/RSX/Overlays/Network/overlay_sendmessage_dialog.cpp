#include "stdafx.h"
#include "../overlay_manager.h"
#include "overlay_sendmessage_dialog.h"
#include "Emu/System.h"
#include "Emu/NP/rpcn_client.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/PPUThread.h" // for vm_var
#include "Emu/Memory/vm_var.h"
#include "Emu/Io/interception.h"

namespace rsx
{
	namespace overlays
	{
		void sendmessage_friend_callback(void* param, rpcn::NotificationType ntype, const std::string& username, bool status)
		{
			auto* dlg = static_cast<sendmessage_dialog*>(param);
			dlg->callback_handler(ntype, username, status);
		}

		sendmessage_dialog::list_entry::list_entry(std::string_view msg)
		{
			std::unique_ptr<overlay_element> text_stack  = std::make_unique<vertical_layout>();
			std::unique_ptr<overlay_element> padding     = std::make_unique<spacer>();
			std::unique_ptr<overlay_element> text_label  = std::make_unique<label>(msg);

			padding->set_size(1, 1);
			text_label->set_size(800, 40);
			text_label->set_font(16);
			text_label->set_wrap_text(true);

			// Make back color transparent for text
			text_label->back_color.a = 0.f;

			static_cast<vertical_layout*>(text_stack.get())->pack_padding = 5;
			static_cast<vertical_layout*>(text_stack.get())->add_element(padding);
			static_cast<vertical_layout*>(text_stack.get())->add_element(text_label);

			// Pack
			pack_padding = 15;
			add_element(text_stack);
		}

		sendmessage_dialog::sendmessage_dialog() : SendMessageDialogBase()
		{
			m_dim_background = std::make_unique<overlay_element>();
			m_dim_background->set_size(virtual_width, virtual_height);
			m_dim_background->back_color.a = 0.5f;

			m_description = std::make_unique<label>();
			m_description->set_font(20);
			m_description->set_pos(20, 37);
			m_description->set_text(get_localized_string(localized_string_id::CELL_NP_SENDMESSAGE_DIALOG_TITLE));
			m_description->auto_resize();
			m_description->back_color.a = 0.f;

			fade_animation.duration_sec = 0.15f;

			return_code = selection_code::canceled;
		}

		void sendmessage_dialog::update(u64 timestamp_us)
		{
			if (fade_animation.active)
			{
				fade_animation.update(timestamp_us);
			}
		}

		void sendmessage_dialog::on_button_pressed(pad_button button_press, bool is_auto_repeat)
		{
			if (fade_animation.active) return;
			if (retro_atomic_load_acquire_int(&m_confirmation_dialog_open) != 0) return; // Ignore input while the confirmation dialog is open

			// The renderer's, as is the list: input and the friends' news reach it there
			bool close_dialog = false;

			switch (button_press)
			{
			case pad_button::cross:
				if (m_list->m_items.empty() || is_auto_repeat)
					break;

				play_sound(sound_effect::accept);

				if (std::string selection = get_current_selection(); !selection.empty())
				{
					// Taken as chosen, for the thread waiting on the dialog
					// (seen by it with the flag below)
					m_chosen_npid = std::move(selection);
					retro_atomic_store_release_int(&m_open_confirmation_dialog, 1);
					libretro_event(); // the dialog's thread waits untimed: woken to open it
					retro_atomic_store_release_int(&m_confirmation_dialog_open, 1); // Ignore input while the confirmation dialog is open. Set this here due to avoid a race condition.
					break;
				}

				return_code = selection_code::error;
				close_dialog = true;
				break;
			case pad_button::circle:
				play_sound(sound_effect::cancel);
				close_dialog = true;
				break;
			case pad_button::dpad_up:
			case pad_button::ls_up:
				m_list->select_previous();
				break;
			case pad_button::dpad_down:
			case pad_button::ls_down:
				m_list->select_next();
				break;
			case pad_button::L1:
				m_list->select_previous(10);
				break;
			case pad_button::R1:
				m_list->select_next(10);
				break;
			default:
				rsx_log.trace("[ui] Button %d pressed", static_cast<u8>(button_press));
				break;
			}

			if (close_dialog)
			{
				fade_animation.current = color4f(1.f);
				fade_animation.end = color4f(0.f);
				fade_animation.active = true;

				fade_animation.on_finish = [this]
				{
					close(true, true);
				};
			}
			// Play a sound unless this is a fast auto repeat which would induce a nasty noise
			else if (!is_auto_repeat || m_auto_repeat_ms_interval >= m_auto_repeat_ms_interval_default)
			{
				play_sound(sound_effect::cursor);
			}
		}

		compiled_resource sendmessage_dialog::get_compiled()
		{
			if (!retro_atomic_load_acquire_int(&visible))
			{
				return {};
			}

			compiled_resource result;
			result.add(m_dim_background->get_compiled());

			if (m_list)
			{
				result.add(m_list->get_compiled());
			}

			result.add(m_description->get_compiled());

			fade_animation.apply(result);

			return result;
		}

		error_code sendmessage_dialog::Exec(message_data& msg_data, std::set<std::string>& npids)
		{
			retro_atomic_store_release_int(&visible, 0);

			localized_string_id confirmation_loc_id = localized_string_id::CELL_NP_SENDMESSAGE_DIALOG_CONFIRMATION;

			switch (msg_data.mainType)
			{
			case SceNpBasicMessageMainType::SCE_NP_BASIC_MESSAGE_MAIN_TYPE_ADD_FRIEND:
				m_description->set_text(get_localized_string(localized_string_id::CELL_NP_SENDMESSAGE_DIALOG_TITLE_ADD_FRIEND));
				m_description->auto_resize();
				confirmation_loc_id = localized_string_id::CELL_NP_SENDMESSAGE_DIALOG_CONFIRMATION_ADD_FRIEND;
				break;
			case SceNpBasicMessageMainType::SCE_NP_BASIC_MESSAGE_MAIN_TYPE_INVITE:
				m_description->set_text(get_localized_string(localized_string_id::CELL_NP_SENDMESSAGE_DIALOG_TITLE_INVITE));
				m_description->auto_resize();
				confirmation_loc_id = localized_string_id::CELL_NP_SENDMESSAGE_DIALOG_CONFIRMATION_INVITE;
				break;
			case SceNpBasicMessageMainType::SCE_NP_BASIC_MESSAGE_MAIN_TYPE_DATA_ATTACHMENT:
			case SceNpBasicMessageMainType::SCE_NP_BASIC_MESSAGE_MAIN_TYPE_GENERAL:
			case SceNpBasicMessageMainType::SCE_NP_BASIC_MESSAGE_MAIN_TYPE_CUSTOM_DATA:
			case SceNpBasicMessageMainType::SCE_NP_BASIC_MESSAGE_MAIN_TYPE_URL_ATTACHMENT:
			default:
				break; // Title already set in constructor
			}

			m_rpcn = rpcn::rpcn_client::get_instance(0, true);

			// Get list of messages
			rpcn::friend_data data;
			m_rpcn->get_friends_and_register_cb(data, sendmessage_friend_callback, this);

			// The list is the renderer's: built there, as are the changes after
			post_call([this, friends = std::move(data.friends)]()
			{
				for (const auto& [name, online_data] : friends)
				{
					// Only add online friends to the list
					if (online_data.online)
					{
						if (std::any_of(m_entry_names.cbegin(), m_entry_names.cend(), [&name](const std::string& entry){ return entry == name; }))
							continue;

						m_entry_names.push_back(name);
					}
				}

				reload({});
			});

			fade_animation.current = color4f(0.f);
			fade_animation.end = color4f(1.f);
			fade_animation.active = true;

			retro_atomic_store_release_int(&visible, 1);

			const auto notify = std::make_shared<retro_atomic_int_t>(0);
			auto& overlayman = g_fxo->get<display_manager>();
			auto& nps = g_fxo->get<np_state>();

			// Block until the user exits the dialog
			overlayman.attach_thread_input(
				uid, "Sendmessage dialog", nullptr,
				[notify](s32) { retro_atomic_store_release_int(notify.get(), 1); retro_waitaddr_notify_one(notify.get()); }
			);

			bool confirmation_error = false;

			while (!Emu.IsStopped() && !retro_atomic_load_acquire_int(notify.get()) && !nps.abort_gui_flag)
			{
				if (retro_atomic_exchange_int(&m_open_confirmation_dialog, 0) != 0)
				{
					// Get user confirmation by opening a blocking dialog
					const std::string npid = m_chosen_npid;
					if (npid.empty())
					{
						rsx_log.fatal("sendmessage dialog can't open confirmation dialog with empty npid");
						confirmation_error = true;
						break;
					}

					rsx_log.notice("sendmessage dialog about to open confirmation dialog");

					const std::string loc_msg = get_localized_string(confirmation_loc_id, npid.c_str());
					const std::string confirmation_msg = fmt::format("%s %s\n\n%s", loc_msg, msg_data.subject, msg_data.body);
					s32 confirmation_code = CELL_MSGDIALOG_BUTTON_NO;

					// Hide list
					retro_atomic_store_release_int(&visible, 0);

					error_code res = open_msg_dialog(true, CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO, vm::make_str(confirmation_msg), msg_dialog_source::_sceNp, vm::null, vm::null, vm::null, &confirmation_code);
					if (res != CELL_OK)
					{
						rsx_log.fatal("sendmessage dialog failed to open confirmation dialog (error=%d)", +res);
						confirmation_error = true;
						break;
					}

					rsx_log.notice("sendmessage dialog received confirmation dialog result %d", confirmation_code);

					if (confirmation_code == CELL_MSGDIALOG_BUTTON_YES)
					{
						return_code = selection_code::ok;
						close(false, true);
						break;
					}

					// Show list again
					retro_atomic_store_release_int(&visible, 1);

					// Allow input again
					retro_atomic_store_release_int(&m_confirmation_dialog_open, 0);

					// Intercept pads again (Only needed because we currently don't have an interception stack and the confirmation dialog disables it on close)
					input::SetIntercepted(true);
				}

				libretro_wait_on(*notify, u32{0});
			}

			m_rpcn->remove_friend_cb(sendmessage_friend_callback, this);

			error_code result = CELL_CANCEL;

			if (confirmation_error)
			{
				rsx_log.error("Sendmessage dialog aborted internally!");
				close(false, true);
				return result;
			}

			if (nps.abort_gui_flag.exchange(false))
			{
				rsx_log.warning("Sendmessage dialog aborted by sceNp!");
				close(false, true);
				return result;
			}

			switch (return_code)
			{
			case selection_code::ok:
			{
				// As the renderer took it, with the press
				const std::string& current_selection = m_chosen_npid;

				if (current_selection.empty())
				{
					rsx_log.fatal("sendmessage dialog can't send message to empty npid");
					break;
				}

				npids.insert(current_selection);

				// Send the message
				if (m_rpcn->send_message(msg_data, npids))
				{
					result = CELL_OK;
				}
				break;
			}
			case selection_code::canceled:
				rsx_log.notice("sendmessage dialog was canceled");
				break;
			default:
				rsx_log.error("sendmessage dialog exited with error: %d", return_code);
				break;
			}

			return result;
		}

		std::string sendmessage_dialog::get_current_selection() const
		{
			if (const s32 index = m_list->get_selected_index(); index >= 0 && static_cast<usz>(index) < m_entry_names.size())
			{
				return m_entry_names[index];
			}

			return {};
		}

		void sendmessage_dialog::reload(std::string_view previous_selection)
		{
			if (m_list)
			{
				status_flags |= status_bits::invalidate_image_cache;
			}
			
			m_list = std::make_unique<list_view>(virtual_width - 2 * 20, 540, false);
			m_list->set_pos(20, 85);

			for (const std::string& name : m_entry_names)
			{
				std::unique_ptr<overlay_element> entry = std::make_unique<list_entry>(name);
				m_list->add_entry(entry);
			}

			if (m_list->m_items.empty())
			{
				m_list->set_cancel_only(true);
			}
			else if (m_list->get_cancel_only())
			{
				m_list->set_cancel_only(false);
				m_list->select_entry(0);
			}
			else
			{
				// Only select an entry if there are entries available
				s32 selected_index = 0;

				// Try to select the previous selection
				if (!previous_selection.empty())
				{
					for (s32 i = 0; i < ::narrow<s32>(m_entry_names.size()); i++)
					{
						if (m_entry_names[i] == previous_selection)
						{
							selected_index = i;
							break;
						}
					}
				}

				m_list->select_entry(selected_index);
			}
		}

		void sendmessage_dialog::callback_handler(rpcn::NotificationType ntype, const std::string& username, bool status)
		{
			// From the server's thread: the list is the renderer's
			post_call([this, ntype, username, status]() { take_friend_news(ntype, username, status); });
		}

		void sendmessage_dialog::take_friend_news(rpcn::NotificationType ntype, const std::string& username, bool status)
		{
			const auto add_friend = [&]()
			{
				if (std::any_of(m_entry_names.cbegin(), m_entry_names.cend(), [&username](const std::string& entry){ return entry == username; }))
					return;

				const std::string current_selection = get_current_selection();
				m_entry_names.push_back(username);
				reload(current_selection);
			};

			const auto remove_friend = [&]()
			{
				const auto it = std::find(m_entry_names.cbegin(), m_entry_names.cend(), username);
				if (it == m_entry_names.cend())
					return;

				const std::string current_selection = get_current_selection();
				m_entry_names.erase(it);
				reload(current_selection);
			};

			switch (ntype)
			{
			case rpcn::NotificationType::FriendQuery: // Other user sent a friend request
			case rpcn::NotificationType::FriendPresenceChanged:
				break;
			case rpcn::NotificationType::FriendNew: // Add a friend to the friendlist(either accepted a friend request or friend accepted it)
			{
				if (status)
				{
					add_friend();
				}
				break;
			}
			case rpcn::NotificationType::FriendLost: // Remove friend from the friendlist(user removed friend or friend removed friend)
			{
				remove_friend();
				break;
			}
			case rpcn::NotificationType::FriendStatus: // Set status of friend to Offline or Online
			{
				if (status)
				{
					add_friend();
				}
				else
				{
					remove_friend();
				}
				break;
			}
			default:
			{
				rsx_log.fatal("An unhandled notification type was received by the sendmessage dialog callback!");
				break;
			}
			}
		}
	} // namespace overlays
} // namespace RSX
