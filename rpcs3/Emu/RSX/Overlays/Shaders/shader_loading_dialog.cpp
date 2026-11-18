#include "stdafx.h"
#include "shader_loading_dialog.h"
#include "Emu/emu_callbacks.h"
#include "Emu/System.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"

#include "util/asm.hpp"

namespace rsx
{
	void shader_loading_dialog::create(const std::string& msg, const std::string& title)
	{
		dlg = g_emu_callbacks.get_msg_dialog();
		if (dlg)
		{
			dlg->type.se_normal = true;
			dlg->type.bg_invisible = true;
			dlg->type.progress_bar_count = 2;
			dlg->ProgressBarSetTaskbarIndex(-1); // -1 to combine all progressbars in the taskbar progress
			dlg->on_close = [](s32 /*status*/)
			{
				Emu.CallFromMainThread([]()
				{
					rsx_log.notice("Aborted shader loading dialog");
					Emu.Kill(false);
				});
			};

			ref_cnt++;

			Emu.CallFromMainThread([&]()
			{
				dlg->Create(msg, title);
				ref_cnt--;
				ref_cnt.notify_all();
			});
		}

		wait_unreferenced();
	}

	void shader_loading_dialog::update_msg(u32 index, std::string msg)
	{
		if (!dlg)
		{
			return;
		}

		ref_cnt++;

		Emu.CallFromMainThread([&, index, message = std::move(msg)]()
		{
			dlg->ProgressBarSetMsg(index, message);
			ref_cnt--;
			ref_cnt.notify_all();
		});
	}

	void shader_loading_dialog::inc_value(u32 index, u32 value)
	{
		if (!dlg)
		{
			return;
		}

		ref_cnt++;

		Emu.CallFromMainThread([&, index, value]()
		{
			dlg->ProgressBarInc(index, value);
			ref_cnt--;
			ref_cnt.notify_all();
		});
	}

	void shader_loading_dialog::set_value(u32 index, u32 value)
	{
		if (!dlg)
		{
			return;
		}

		ref_cnt++;

		Emu.CallFromMainThread([&, index, value]()
		{
			dlg->ProgressBarSetValue(index, value);
			ref_cnt--;
			ref_cnt.notify_all();
		});
	}

	void shader_loading_dialog::set_limit(u32 index, u32 limit)
	{
		if (!dlg)
		{
			return;
		}

		ref_cnt++;

		Emu.CallFromMainThread([&, index, limit]()
		{
			dlg->ProgressBarSetLimit(index, limit);
			ref_cnt--;
			ref_cnt.notify_all();
		});
	}

	// The main thread's calls done, or the emulator stopping (which skips
	// them): woken by either (ref_cnt, g_libretro_events), no spin
	void shader_loading_dialog::wait_unreferenced()
	{
		for (;;)
		{
			const int refs = ref_cnt.load();
			const u32 events = libretro_events_seen();

			if (!refs || Emu.IsStopped())
			{
				return;
			}

			atomic_wait::list<2> list;
			list.template set<0>(ref_cnt, refs);
			list.template set_raw<1>(&g_libretro_events, events);
			list.wait();
		}
	}

	void shader_loading_dialog::refresh()
	{
	}

	void shader_loading_dialog::close()
	{
		wait_unreferenced();
	}
}
