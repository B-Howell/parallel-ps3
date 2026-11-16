#include "stdafx.h"
#include "swapchain.h"
#include "Emu/system_config.h"

namespace vk
{
	// Swapchain image RPCS3
	swapchain_image_RPCS3::swapchain_image_RPCS3(render_device& dev, const memory_type_mapping& memory_map, u32 width, u32 height)
		:image(dev, memory_map.device_local, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, width, height, 1, 1, 1,
			VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_TILING_OPTIMAL,
			// Sampled as well: with the libretro core's hardware context the
			// frontend draws from it (set_image wants TRANSFER_SRC and SAMPLED).
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, VMM_ALLOCATION_POOL_SWAPCHAIN)
	{
		m_width = width;
		m_height = height;
		current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

		m_dma_buffer = std::make_unique<buffer>(dev, m_width * m_height * 4, memory_map.host_visible_coherent,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, VMM_ALLOCATION_POOL_SWAPCHAIN);
	}

	void swapchain_image_RPCS3::do_dma_transfer(command_buffer& cmd)
	{
		VkBufferImageCopy copyRegion = {};
		copyRegion.bufferOffset = 0;
		copyRegion.bufferRowLength = m_width;
		copyRegion.bufferImageHeight = m_height;
		copyRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copyRegion.imageOffset = {};
		copyRegion.imageExtent = { m_width, m_height, 1 };

		change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		vkCmdCopyImageToBuffer(cmd, value, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_dma_buffer->value, 1, &copyRegion);
		change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	}

	u32 swapchain_image_RPCS3::get_required_memory_size() const
	{
		return m_width * m_height * 4;
	}

	void* swapchain_image_RPCS3::get_pixels()
	{
		return m_dma_buffer->map(0, VK_WHOLE_SIZE);
	}

	void swapchain_image_RPCS3::free_pixels()
	{
		m_dma_buffer->unmap();
	}

	// swapchain BASE
	swapchain_base::swapchain_base(physical_device& gpu, u32 present_queue, u32 graphics_queue, u32 transfer_queue, VkFormat format)
	{
		dev.create(gpu, graphics_queue, present_queue, transfer_queue);
		m_surface_format = format;
	}

	// NATIVE swapchain base
	VkResult native_swapchain_base::acquire_next_swapchain_image(VkSemaphore /*semaphore*/, u64 /*timeout*/, u32* result)
	{
		u32 index = 0;
		for (auto& p : swapchain_images)
		{
			if (!p.first)
			{
				p.first = true;
				*result = index;
				return VK_SUCCESS;
			}

			++index;
		}

		return VK_NOT_READY;
	}

	void native_swapchain_base::init_swapchain_images(render_device& dev, u32 preferred_count)
	{
		swapchain_images.resize(preferred_count);
		for (auto& img : swapchain_images)
		{
			img.second = std::make_unique<swapchain_image_RPCS3>(dev, dev.get_memory_mapping(), m_width, m_height);
			img.first = false;
		}
	}
}
