#pragma once

#include <cstring>
#include <map>

#include "Emu/Memory/vm_ptr.h"
#include "np_pool.h"
#include "util/asm.hpp"
#include "util/logs.hpp"

LOG_CHANNEL(np_mem_allocator);

namespace np
{
	// The NP libraries' pool of guest memory (sceNpInit's), handed out from
	// any thread with no lock and no thread waiting for another (np_pool.h).
	// Set up and released by sceNpInit and sceNpTerm, with nothing allocating
	class memory_allocator
	{
	public:
		memory_allocator() = default;
		memory_allocator(utils::serial& ar) noexcept { save(ar); }
		memory_allocator(const memory_allocator&) = delete;
		memory_allocator& operator=(const memory_allocator&) = delete;
		~memory_allocator() { np_pool_deinit(&m_free); }

		void save(utils::serial& ar);

		void setup(vm::ptr<void> ptr_pool, u32 size)
		{
			np_pool_deinit(&m_free);
			m_pool = ptr_pool;
			ensure(np_pool_init(&m_free, size));
		}

		void release()
		{
			m_pool = vm::null;
			np_pool_deinit(&m_free);
		}

		std::tuple<u32, u32, u32> get_stats() const
		{
			const u32 size = np_pool_size(&m_free);
			return {size, size - np_pool_avail(&m_free), np_pool_max_used(&m_free)};
		}

		u32 allocate(u32 size)
		{
			if (!size)
			{
				np_mem_allocator.error("Can't allocate 0 size buffer!");
				return 0;
			}

			u32 offset = 0;
			if (!np_pool_alloc(&m_free, size, &offset))
			{
				np_mem_allocator.error("Not enough memory available in NP pool!");
				return 0;
			}

			// The whole granules, as the guest is given them
			const u32 alloc_size = utils::align(size, NP_POOL_GRANULE);
			std::memset(static_cast<u8*>(m_pool.get_ptr()) + offset, 0, alloc_size);

			np_mem_allocator.trace("Allocation off:%d size:%d", offset, alloc_size);
			return m_pool.addr() + offset;
		}

		void free(u32 addr)
		{
			ensure(addr >= m_pool.addr() && addr < (m_pool.addr() + np_pool_size(&m_free)), "memory_allocator::free: addr is out of bounds!");
			ensure(np_pool_free(&m_free, addr - m_pool.addr()), "memory_allocator::free: no allocation starts there!");
		}

		void shrink_allocation(u32 addr, u32 new_size)
		{
			ensure(addr >= m_pool.addr() && addr < (m_pool.addr() + np_pool_size(&m_free)), "memory_allocator::reduce_allocation: addr is out of bounds!");
			ensure(np_pool_shrink(&m_free, addr - m_pool.addr(), new_size), "memory_allocator::reduce_allocation: no allocation that large starts there!");
		}

	private:
		vm::ptr<void> m_pool{};
		np_pool_t m_free{};
	};
} // namespace np
