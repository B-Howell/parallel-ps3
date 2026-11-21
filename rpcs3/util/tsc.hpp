#pragma once

#include "util/types.hpp"
#include <retro_atomic.h>

#ifdef _M_X64
#ifdef _MSC_VER
extern "C" u64 __rdtsc();
#else
#include <immintrin.h>
#endif
#endif

namespace utils
{
	inline u64 get_tsc()
	{
#if defined(ARCH_ARM64)
		u64 r = 0;
		__asm__ volatile("mrs %0, cntvct_el0" : "=r" (r));
		return r;
#elif defined(_M_X64)
		return __rdtsc();
#elif defined(ARCH_X64)
		return __builtin_ia32_rdtsc();
#else
#error "Missing utils::get_tsc() implementation"
#endif
	}

	// A TSC stamp no other call in this process returns (for unique file
	// names): the stamp, or one past the last given when the stamp is not
	// past it
	inline u64 get_unique_tsc()
	{
		static retro_atomic_64_t s_last;
		const u64 stamp = get_tsc();
		s64 seen = retro_atomic_load_acquire_64(&s_last);

		while (stamp > static_cast<u64>(seen))
		{
			if (retro_atomic_cas_64(&s_last, seen, static_cast<s64>(stamp)))
			{
				return stamp;
			}

			seen = retro_atomic_load_acquire_64(&s_last);
		}

		return static_cast<u64>(retro_atomic_fetch_add_64(&s_last, 1)) + 1;
	}
}
