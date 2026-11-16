#pragma once

#include "GSFrameBase.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/system_config_types.h"

class GSRender : public rsx::thread
{
protected:
	GSFrameBase* m_frame;
	draw_context_t m_context = nullptr;
	bool m_continuous_mode = false;

	vsync_mode m_vsync_mode{};

public:
	~GSRender() override;

	GSRender(utils::serial* ar) noexcept;

	void set_continuous_mode(bool continuous_mode) { m_continuous_mode = continuous_mode; }

	void on_init_thread() override;
	void on_exit() override;

	void flip(const rsx::display_flip_info_t& info) override;
	f64 get_display_refresh_rate() const override;

	GSFrameBase* get_frame() const { return m_frame; }
};
