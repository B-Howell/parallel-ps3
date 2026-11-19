#pragma once

#include "../overlays.h"
#include <retro_atomic.h>
#include "../overlay_list_view.hpp"
#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/Modules/sceNp.h"

namespace rsx
{
	namespace overlays
	{
		struct sendmessage_dialog : public user_interface, public SendMessageDialogBase
		{
		private:
			struct list_entry : horizontal_layout
			{
			public:
				list_entry(std::string_view msg);
			};

			std::vector<std::string> m_entry_names;
			std::string m_chosen_npid; // set before m_open_confirmation_dialog, as the press is taken
			std::unique_ptr<overlay_element> m_dim_background;
			std::unique_ptr<list_view> m_list;
			std::unique_ptr<label> m_description;
			retro_atomic_int_t m_open_confirmation_dialog{0}; // 0 or 1
			retro_atomic_int_t m_confirmation_dialog_open{0}; // 0 or 1

			animation_color_interpolate fade_animation;

			std::string get_current_selection() const;
			void reload(std::string_view previous_selection);
			void take_friend_news(rpcn::NotificationType ntype, const std::string& username, bool status);

		public:
			sendmessage_dialog();

			void update(u64 timestamp_us) override;
			void on_button_pressed(pad_button button_press, bool is_auto_repeat) override;

			compiled_resource get_compiled() override;

			error_code Exec(message_data& msg_data, std::set<std::string>& npids) override;
			void callback_handler(rpcn::NotificationType ntype, const std::string& username, bool status) override;
		};
	}
}
