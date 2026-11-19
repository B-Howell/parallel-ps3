#pragma once

#include <util/types.hpp>
#include <retro_atomic.h>
#include "Utilities/mutex.h"
#include "Emu/CPU/CPUThread.h"

namespace rsx
{
	namespace iomap_helper
	{
		template <bool Shared>
		struct io_lock
		{
			shared_mutex& ref;

			io_lock(shared_mutex& obj)
				: ref(obj)
			{}

			bool try_lock()
			{
				if constexpr (Shared)
				{
					return ref.try_lock_shared();
				}

				return ref.try_lock();
			}

			void lock()
			{
				if constexpr (Shared)
				{
					ref.lock_shared();
					return;
				}

				ref.lock();
			}
		};
	}

	struct rsx_iomap_table
	{
		static constexpr u32 c_lock_stride = 8192;

		// Each a u32 in a retro_atomic word, umax where nothing is mapped:
		// ea[io >> 20] is the effective address an IO megabyte maps to, and
		// io[ea >> 20] the IO address an effective megabyte is mapped at
		mutable std::array<retro_atomic_int_t, 4096> ea;
		mutable std::array<retro_atomic_int_t, 4096> io;

		u32 get_ea(u32 io_mb) const noexcept { return static_cast<u32>(retro_atomic_load_acquire_int(&ea[io_mb])); }
		u32 get_io(u32 ea_mb) const noexcept { return static_cast<u32>(retro_atomic_load_acquire_int(&io[ea_mb])); }
		void set_ea(u32 io_mb, u32 value) noexcept { retro_atomic_store_release_int(&ea[io_mb], static_cast<int>(value)); }
		void set_io(u32 ea_mb, u32 value) noexcept { retro_atomic_store_release_int(&io[ea_mb], static_cast<int>(value)); }
		std::array<shared_mutex, 0x1'0000'0000 / c_lock_stride> rs;

		rsx_iomap_table() noexcept;

		// Try to get the real address given a mapped address
		// Returns -1 on failure
		u32 get_addr(u32 offs) const noexcept
		{
			return get_ea(offs >> 20) | (offs & 0xFFFFF);
		}

		template <bool IsFullLock, uint Stride>
		bool lock(u32 addr, u32 len, cpu_thread* self = nullptr) noexcept
		{
			if (len <= 1) return false;
			const u32 end = addr + len - 1;

			bool added_wait = false;

			for (u32 block = addr / c_lock_stride; block <= (end / c_lock_stride); block += Stride)
			{
				auto mutex_ = iomap_helper::io_lock<!IsFullLock>(rs[block]);

				if (!mutex_.try_lock()) [[ unlikely ]]
				{
					if (self)
					{
						added_wait |= !self->state.test_and_set(cpu_flag::wait);
					}

					if (!self || self->get_class() != thread_class::rsx)
					{
						mutex_.lock();
						continue;
					}

					while (!mutex_.try_lock())
					{
						self->cpu_wait({});
					}
				}
			}

			if (added_wait)
			{
				self->check_state();
			}

			return true;
		}

		template <bool IsFullLock, uint Stride>
		void unlock(u32 addr, u32 len) noexcept
		{
			ensure(len >= 1);
			const u32 end = addr + len - 1;

			for (u32 block = (addr / 8192); block <= (end / 8192); block += Stride)
			{
				if constexpr (IsFullLock)
				{
					rs[block].unlock();
				}
				else
				{
					rs[block].unlock_shared();
				}
			}
		}
	};
}
