#pragma once

#include "shared.h"
#include "../../Utils/rsx_utils.h"

#include <queues/mpsc_stack.h>

namespace vk
{
	class query_pool : public rsx::ref_counted
	{
		VkQueryPool m_query_pool;
		VkDevice m_device;
		u32 m_size;

	public:
		// Its link while it waits, released, to be cached again by the
		// renderer (query_pool_manager): the link first, then the pool
		struct released_node
		{
			mpsc_stack_node_t link;
			query_pool* pool;
		} released{{}, this};

		query_pool(VkDevice dev, VkQueryType type, u32 size)
			: m_query_pool(VK_NULL_HANDLE)
			, m_device(dev)
			, m_size(size)
		{
			VkQueryPoolCreateInfo info{};
			info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
			info.queryType  = type;
			info.queryCount = size;
			CHECK_RESULT(vkCreateQueryPool(dev, &info, nullptr, &m_query_pool));

			reset_refs();
		}

		~query_pool()
		{
			vkDestroyQueryPool(m_device, m_query_pool, nullptr);
		}

		void reset_refs()
		{
			// Take 'size' references on this object
			ref_count.release(static_cast<s32>(m_size));
		}

		operator VkQueryPool()
		{
			return m_query_pool;
		}

		inline u32 size() const
		{
			return m_size;
		}
	};
}
