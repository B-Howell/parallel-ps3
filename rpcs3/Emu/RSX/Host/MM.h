#pragma once

#include <util/types.hpp>
#include <util/vm.hpp>

namespace rsx
{
	// Protects host memory for the texture cache, at once
	void mm_protect(void* start, u64 length, utils::protection prot);
}
