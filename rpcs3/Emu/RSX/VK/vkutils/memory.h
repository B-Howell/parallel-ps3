#pragma once

#include "shared.h"

#include "Emu/RSX/Utils/rsx_utils.h"

#include "vk_heap.h"

#include <memory>

namespace vk
{
	namespace vmm_allocation_pool_ // Workaround for clang < 13 not supporting enum imports
	{
		enum vmm_allocation_pool
		{
			VMM_ALLOCATION_POOL_UNDEFINED = 0,
			VMM_ALLOCATION_POOL_SYSTEM,
			VMM_ALLOCATION_POOL_SURFACE_CACHE,
			VMM_ALLOCATION_POOL_TEXTURE_CACHE,
			VMM_ALLOCATION_POOL_SWAPCHAIN,
			VMM_ALLOCATION_POOL_SCRATCH,
			VMM_ALLOCATION_POOL_SAMPLER,
		};
	}

	using namespace vk::vmm_allocation_pool_;

	class render_device;
	class memory_type_info
	{
		std::vector<u32> type_ids;
		std::vector<u64> type_sizes;

	public:
		memory_type_info() = default;
		memory_type_info(u32 index, u64 size);
		void push(u32 index, u64 size);

		using iterator = u32*;
		using const_iterator = const u32*;
		const_iterator begin() const;
		const_iterator end() const;
		u32 first() const;
		size_t count() const;
		u64 total_bytes() const;

		operator bool() const;
		bool operator == (const memory_type_info& other) const;

		memory_type_info get(const render_device& dev, u32 access_flags, u32 type_mask) const;

		void rebalance();
	};

	struct memory_heap_info
	{
		u32 index;
		u32 flags;
		u64 size;
	};

	struct memory_allocation_request
	{
		u64 size = 0;
		u64 alignment = 1;
		const memory_type_info* memory_type = nullptr;
		vmm_allocation_pool pool = VMM_ALLOCATION_POOL_UNDEFINED;
		bool throw_on_fail = true;
		bool recover_vmem_on_fail = true;
		// A buffer or a linear image; an optimal image is not. The two never
		// share a block (bufferImageGranularity)
		bool linear = true;
	};

	// Device memory: the renderer's own blocks, handed out as offsets in them
	// (vk_heap.h, in place of the Vulkan Memory Allocator), from any thread
	// with no lock and no thread waiting for another: a thread that finds the
	// blocks another's takes a VkDeviceMemory of its own. With "Disable Vulkan
	// Memory Allocator" every allocation is a VkDeviceMemory of its own, as it was
	class mem_allocator
	{
	public:
		mem_allocator(const vk::render_device& dev, VkPhysicalDevice pdev);
		~mem_allocator();

		// out.memory is VK_NULL_HANDLE when there was no memory and the
		// request does not throw. memory_type is the type it came from
		void alloc(const memory_allocation_request& request, u32& memory_type, vk_heap_alloc_t& out);
		void free(const vk_heap_alloc_t& alloc);

		// The fullest driver heap's use of its budget, in percent
		f32 get_memory_usage();

		mem_allocator(const mem_allocator&) = delete;
		mem_allocator& operator=(const mem_allocator&) = delete;

	private:
		VkResult try_alloc(const memory_allocation_request& request, u32& memory_type, vk_heap_alloc_t& out);

		std::unique_ptr<vk_heap_t> m_heap;
		u32 m_rebar_heap_idx = UINT32_MAX;
	};

	struct memory_block
	{
		memory_block(VkDevice dev, const memory_allocation_request& alloc_request);
		virtual ~memory_block();

		virtual VkDeviceMemory get_vk_device_memory();
		virtual u64 get_vk_device_memory_offset();

		virtual void* map(u64 offset, u64 size);
		virtual void unmap();

		u64 size() const;

		memory_block(const memory_block&) = delete;
		memory_block(memory_block&&)      = delete;

	protected:
		memory_block() = default;

	private:
		VkDevice m_device;
		vk::mem_allocator* m_mem_allocator = nullptr;
		vk_heap_alloc_t m_alloc{};
		u64 m_size;

		// What the allocation counted against, given back on its free: the
		// totals are counters, with no table of allocations to look it up in
		u32 m_memory_type = ~0u;
		vmm_allocation_pool m_pool = VMM_ALLOCATION_POOL_UNDEFINED;
	};

	struct memory_block_host : public memory_block
	{
		memory_block_host(VkDevice dev, void* host_pointer, u64 size, const memory_type_info& memory_type);
		~memory_block_host();

		VkDeviceMemory get_vk_device_memory() override;
		u64 get_vk_device_memory_offset() override;
		void* map(u64 offset, u64 size) override;
		void unmap() override;

		memory_block_host(const memory_block_host&) = delete;
		memory_block_host(memory_block_host&&) = delete;
		memory_block_host() = delete;

	private:
		VkDevice m_device;
		VkDeviceMemory m_mem_handle;
		void* m_host_pointer;
	};

	// Tracking for memory usage. Constrained largely by amount of VRAM + shared video memory.
	void vmm_notify_memory_allocated(u32 memory_type, u64 memory_size, vmm_allocation_pool pool);
	void vmm_notify_memory_freed(u32 memory_type, u64 memory_size, vmm_allocation_pool pool);
	void vmm_reset();
	void vmm_check_memory_usage();
	u64  vmm_get_application_memory_usage(const memory_type_info& memory_type);
	u64  vmm_get_application_pool_usage(vmm_allocation_pool pool);
	bool vmm_handle_memory_pressure(rsx::problem_severity severity);
	rsx::problem_severity vmm_determine_memory_load_severity();

	// Tracking for host memory objects. Allocated count is more important than actual memory amount.
	void vmm_notify_object_allocated(vmm_allocation_pool pool);
	void vmm_notify_object_freed(vmm_allocation_pool pool);

	mem_allocator* get_current_mem_allocator();
}
