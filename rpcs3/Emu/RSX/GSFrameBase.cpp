#include "stdafx.h"
#include "GSFrameBase.h"
#include "Emu/system_config.h"

// The frontend owns the window and its focus: nothing in the core ever
// set the game-window-focused flag this read, so it was always false and
// is gone. Unchanged: the setting alone decides
bool is_input_allowed()
{
	return g_cfg.io.background_input_enabled.get();
}
