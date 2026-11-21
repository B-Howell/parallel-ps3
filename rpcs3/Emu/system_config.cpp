#include "stdafx.h"
#include "system_config.h"

#include <crypto/kdf.h>

cfg_root g_cfg{};
cfg_root g_backup_cfg{};

std::string cfg_root::node_sys::get_random_system_name()
{
	// From libretro-common's entropy source, where it was rand() seeded by the
	// wall clock; 0 when no source answers
	u32 r = 0;
	crypto_random_bytes(reinterpret_cast<uint8_t*>(&r), sizeof(r));
	return "RPCS3-" + std::to_string(100 + r % 899);
}

u128 cfg_root::node_sys::get_random_psid()
{
	// 128 bits from libretro-common's entropy source, where each 32 bits
	// came from an engine seeded by the standard library's random device
	u128 result{};
	crypto_random_bytes(reinterpret_cast<uint8_t*>(&result), sizeof(result));

	return result;
}
