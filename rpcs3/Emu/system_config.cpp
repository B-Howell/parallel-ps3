#include "stdafx.h"
#include "system_config.h"

#include <random>
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
	// Generate seed for each 32-bits for maximum entropy using Standard Library
	std::uniform_int_distribution<u32> uniform_dist;
	std::mt19937 prng;

	u128 result{};
	prng.seed(std::random_device()()), result += u128{uniform_dist(prng)} << 0;
	prng.seed(std::random_device()()), result += u128{uniform_dist(prng)} << 32;
	prng.seed(std::random_device()()), result += u128{uniform_dist(prng)} << 64;
	prng.seed(std::random_device()()), result += u128{uniform_dist(prng)} << 96;

	return result;
}
