#include "device.h"
#include "memory.h"

#include "Emu/RSX/Utils/algorithm.hpp"
#include "Emu/system_config.h"


namespace vk
{
	memory_type_info::memory_type_info(u32 index, u64 size)
	{
		push(index, size);
	}
	void memory_type_info::push(u32 index, u64 size)
	{
		type_ids.push_back(index);
		type_sizes.push_back(size);
	}

	memory_type_info::const_iterator memory_type_info::begin() const
	{
		return type_ids.data();
	}

	memory_type_info::const_iterator memory_type_info::end() const
	{
		return type_ids.data() + type_ids.size();
	}

	u32 memory_type_info::first() const
	{
		ensure(!type_ids.empty());
		return type_ids.front();
	}

	size_t memory_type_info::count() const
	{
		return type_ids.size();
	}

	u64 memory_type_info::total_bytes() const
	{
		u64 result = 0;
		for (const auto& size : type_sizes)
		{
			result += size;
		}
		return result;
	}

	memory_type_info::operator bool() const
	{
		return !type_ids.empty();
	}

	bool memory_type_info::operator == (const memory_type_info& other) const
	{
		if (type_ids.size() != other.type_ids.size())
		{
			return false;
		}

		switch (type_ids.size())
		{
		case 1:
			return type_ids[0] == other.type_ids[0];
		case 2:
			return ((type_ids[0] == other.type_ids[0] && type_ids[1] == other.type_ids[1]) ||
					(type_ids[0] == other.type_ids[1] && type_ids[1] == other.type_ids[0]));
		default:
			for (const auto& id : other.type_ids)
			{
				if (std::find(type_ids.begin(), type_ids.end(), id) == type_ids.end())
				{
					return false;
				}
			}
			return true;
		}
	}

	memory_type_info memory_type_info::get(const render_device& dev, u32 access_flags, u32 type_mask) const
	{
		memory_type_info result{};
		for (size_t i = 0; i < type_ids.size(); ++i)
		{
			if (type_mask & (1 << type_ids[i]))
			{
				result.push(type_ids[i], type_sizes[i]);
			}
		}

		if (!result)
		{
			u32 type;
			if (dev.get_compatible_memory_type(type_mask, access_flags, &type))
			{
				result = { type, 0ull };
			}
		}

		return result;
	}

	void memory_type_info::rebalance()
	{
		// Re-order indices with the least used one first.
		// This will avoid constant pressure on the memory budget in low memory systems.

		if (type_ids.size() <= 1)
		{
			// Nothing to do
			return;
		}

		std::vector<std::pair<u32, u64>> free_memory_map;
		const auto num_types = type_ids.size();
		u64 last_free = UINT64_MAX;
		bool to_reorder = false;

		for (u32 i = 0; i < num_types; ++i)
		{
			const auto heap_size = type_sizes[i];
			const auto type_id = type_ids[i];
			ensure(heap_size > 0);

			const u64 used_mem = vmm_get_application_memory_usage({ type_id, 0ull });
			const u64 free_mem = (used_mem >= heap_size) ? 0ull : (heap_size - used_mem);

			to_reorder |= (free_mem > last_free);
			last_free = free_mem;

			free_memory_map.push_back({ i, free_mem });
		}

		if (!to_reorder) [[likely]]
		{
			return;
		}

		ensure(free_memory_map.size() == num_types);
		std::sort(free_memory_map.begin(), free_memory_map.end(), FN(x.second > y.second));

		std::vector<u32> new_type_ids(num_types);
		std::vector<u64> new_type_sizes(num_types);

		for (u32 i = 0; i < num_types; ++i)
		{
			const u32 ref = free_memory_map[i].first;
			new_type_ids[i] = type_ids[ref];
			new_type_sizes[i] = type_sizes[ref];
		}

		type_ids = new_type_ids;
		type_sizes = new_type_sizes;

		rsx_log.warning("Rebalanced memory types successfully");
	}

	mem_allocator::mem_allocator(const vk::render_device& dev, VkPhysicalDevice pdev)
		: m_heap(std::make_unique<vk_heap_t>())
	{
		VkPhysicalDeviceMemoryProperties memory_properties;
		vkGetPhysicalDeviceMemoryProperties(pdev, &memory_properties);

		// The VRAM limit, on every device-local heap
		VkDeviceSize heap_limits[VK_MAX_MEMORY_HEAPS] = {};
		const auto vram_allocation_limit = g_cfg.video.vk.vram_allocation_limit * 0x100000ull;
		if (vram_allocation_limit < dev.get_memory_mapping().device_local_total_bytes)
		{
			for (u32 i = 0; i < memory_properties.memoryHeapCount; ++i)
			{
				if (memory_properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
				{
					heap_limits[i] = vram_allocation_limit;
				}
			}
		}

		const vk_heap_fns_t fns{ vkAllocateMemory, vkFreeMemory, vkMapMemory, vkUnmapMemory };
		vk_heap_init(m_heap.get(), dev, &memory_properties, &fns, 0, heap_limits,
			g_cfg.video.disable_vulkan_mem_allocator ? 1 : 0);

		// Determine the rebar heap. We will exclude it from stats
		const auto& memory_map = dev.get_memory_mapping();
		if (memory_map.device_bar_total_bytes !=
			memory_map.device_local_total_bytes)
		{
			for (u32 i = 0; i < ::size32(memory_map.heaps); ++i)
			{
				const auto& heap = memory_map.heaps[i];
				if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
					heap.size == memory_map.device_bar_total_bytes)
				{
					m_rebar_heap_idx = i;
					break;
				}
			}
		}
	}

	// The device is going: nothing allocates or frees any more
	mem_allocator::~mem_allocator()
	{
		vk_heap_shutdown(m_heap.get());
	}

	VkResult mem_allocator::try_alloc(const memory_allocation_request& request, u32& memory_type, vk_heap_alloc_t& out)
	{
		const u64 size = rsx::align2(request.size, request.alignment);

		// Any thread, none waiting for another (vk_heap.h)
		return vk_heap_alloc_shared(m_heap.get(), request.memory_type->begin(), static_cast<unsigned>(request.memory_type->count()),
			size, request.alignment, request.linear ? 1 : 0, &out, &memory_type);
	}

	void mem_allocator::alloc(const memory_allocation_request& request, u32& memory_type, vk_heap_alloc_t& out)
	{
		// On successful allocation, simply tag the transaction and carry on.
		VkResult error_code = try_alloc(request, memory_type, out);
		if (error_code == VK_SUCCESS)
		{
			vmm_notify_memory_allocated(memory_type, request.size, request.pool);
			return;
		}

		// Out of the heap's turn: what is evicted to make room frees in it
		if (request.recover_vmem_on_fail)
		{
			const auto severity = (request.throw_on_fail) ? rsx::problem_severity::fatal : rsx::problem_severity::severe;
			if (error_code == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
				vmm_handle_memory_pressure(severity))
			{
				// Out of memory. Try again.
				error_code = try_alloc(request, memory_type, out);
				if (error_code == VK_SUCCESS)
				{
					rsx_log.warning("Renderer ran out of video memory but successfully recovered.");
					vmm_notify_memory_allocated(memory_type, request.size, request.pool);
					return;
				}
			}
		}

		out = {};
		if (!request.throw_on_fail)
		{
			return;
		}

		die_with_error(error_code);
		fmt::throw_exception("Unreachable! Error_code=0x%x", static_cast<u32>(error_code));
	}

	void mem_allocator::free(const vk_heap_alloc_t& alloc)
	{
		vk_heap_free_shared(m_heap.get(), &alloc);
	}

	f32 mem_allocator::get_memory_usage()
	{
		// What is taken from each driver heap against four fifths of it (or
		// of its limit), as the Vulkan Memory Allocator reckoned it
		float max_usage = 0.f;
		for (u32 i = 0; i < m_heap->props.memoryHeapCount; ++i)
		{
			// Filter out the Re-BAR heap
			if (i == m_rebar_heap_idx)
			{
				continue;
			}

			u64 size = m_heap->props.memoryHeaps[i].size;
			if (m_heap->heap_limit[i] && m_heap->heap_limit[i] < size)
			{
				size = m_heap->heap_limit[i];
			}

			const u64 budget = size * 8 / 10;
			if (!budget)
			{
				continue;
			}

			const float this_usage = (vk_heap_reserved(m_heap.get(), i) * 100.f) / budget;
			max_usage = std::max(max_usage, this_usage);
		}

		return max_usage;
	}

	mem_allocator* get_current_mem_allocator()
	{
		return g_render_device->get_allocator();
	}

	memory_block::memory_block(VkDevice dev, const memory_allocation_request& alloc_request)
		: m_device(dev), m_size(alloc_request.size), m_pool(alloc_request.pool)
	{
		m_mem_allocator = get_current_mem_allocator();
		m_mem_allocator->alloc(alloc_request, m_memory_type, m_alloc);
	}

	memory_block::~memory_block()
	{
		if (m_mem_allocator && m_alloc.memory != VK_NULL_HANDLE)
		{
			vmm_notify_memory_freed(m_memory_type, m_size, m_pool);
			m_mem_allocator->free(m_alloc);
		}
	}

	memory_block_host::memory_block_host(VkDevice dev, void* host_pointer, u64 size, const memory_type_info& memory_type) :
		m_device(dev), m_mem_handle(VK_NULL_HANDLE), m_host_pointer(host_pointer)
	{
		VkMemoryAllocateInfo alloc_info{};
		VkImportMemoryHostPointerInfoEXT import_info{};

		alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc_info.memoryTypeIndex = memory_type.first();
		alloc_info.allocationSize = size;
		alloc_info.pNext = &import_info;

		import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
		import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
		import_info.pHostPointer = host_pointer;

		CHECK_RESULT(vkAllocateMemory(m_device, &alloc_info, nullptr, &m_mem_handle));
	}

	memory_block_host::~memory_block_host()
	{
		vkFreeMemory(m_device, m_mem_handle, nullptr);
	}

	VkDeviceMemory memory_block_host::get_vk_device_memory()
	{
		return m_mem_handle;
	}

	u64 memory_block_host::get_vk_device_memory_offset()
	{
		return 0ull;
	}

	void* memory_block_host::map(u64 offset, u64 /*size*/)
	{
		return reinterpret_cast<char*>(m_host_pointer) + offset;
	}

	void memory_block_host::unmap()
	{
		// NOP
	}

	VkDeviceMemory memory_block::get_vk_device_memory()
	{
		return m_alloc.memory;
	}

	u64 memory_block::get_vk_device_memory_offset()
	{
		return m_alloc.offset;
	}

	u64 memory_block::size() const
	{
		return m_size;
	}

	// Host-visible blocks are mapped for good when they are taken
	void* memory_block::map(u64 offset, u64 /*size*/)
	{
		ensure(m_alloc.mapped);
		return static_cast<u8*>(m_alloc.mapped) + offset;
	}

	void memory_block::unmap()
	{
	}
}
