#include "unique_resource.h"
#include <retro_atomic.h>

namespace vk
{
	static retro_atomic_64_t s_resource_uid{0};

	u64 gen_uid()
	{
		return static_cast<u64>(retro_atomic_fetch_add_64(&s_resource_uid, 1));
	}
}
