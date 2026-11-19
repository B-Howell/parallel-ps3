#pragma once

#include <retro_atomic.h>

class MsgDialogBase;

namespace rsx
{
	struct shader_loading_dialog
	{
		std::shared_ptr<MsgDialogBase> dlg{};
		retro_atomic_int_t ref_cnt{0};

		void wait_unreferenced();

		virtual ~shader_loading_dialog() = default;
		virtual void create(const std::string& msg, const std::string& title);
		virtual void update_msg(u32 index, std::string msg);
		virtual void inc_value(u32 index, u32 value);
		virtual void set_value(u32 index, u32 value);
		virtual void set_limit(u32 index, u32 limit);
		virtual void refresh();
		virtual void close();
	};
}
