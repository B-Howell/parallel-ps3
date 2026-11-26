#include "barriers.h"
#include "buffer_object.h"
#include "image.h"

#include "../VKResourceManager.h"

#include <util/asm.hpp>
#include "util/logs.hpp"
#include "util/key_list.h"

namespace vk
{
	// By view type, the five there are (1D, 2D, 3D, cube, 2D array): an array, where it was an STL hash map
	std::unique_ptr<viewable_image> g_null_image_views[VK_IMAGE_VIEW_TYPE_2D_ARRAY + 1];
	// By format and class, the few in use: a short list (util/key_list.c), owned, where it was an STL hash map
	key_list_t g_typeless_textures{};
	VkSampler g_null_sampler = nullptr;

	// Scratch memory handling. Use double-buffered resource to significantly cut down on GPU stalls
	struct scratch_buffer_pool_t
	{
		std::array<std::unique_ptr<buffer>, 2> scratch_buffers;
		u32 current_index = 0;

		std::unique_ptr<buffer>& get_buf()
		{
			auto& ret = scratch_buffers[current_index];
			current_index ^= 1;
			return ret;
		}
	};

	// By queue family, an index the device gives (a few): an array, where it was an STL hash map asked on each
	// scratch use
	constexpr u32 max_scratch_queue_families = 16;
	scratch_buffer_pool_t g_scratch_buffers_pool[max_scratch_queue_families];

	VkSampler null_sampler()
	{
		if (g_null_sampler)
			return g_null_sampler;

		VkSamplerCreateInfo sampler_info = {};

		sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		sampler_info.anisotropyEnable = VK_FALSE;
		sampler_info.compareEnable = VK_FALSE;
		sampler_info.unnormalizedCoordinates = VK_FALSE;
		sampler_info.mipLodBias = 0;
		sampler_info.maxAnisotropy = 0;
		sampler_info.magFilter = VK_FILTER_NEAREST;
		sampler_info.minFilter = VK_FILTER_NEAREST;
		sampler_info.compareOp = VK_COMPARE_OP_NEVER;
		sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

		vkCreateSampler(*g_render_device, &sampler_info, nullptr, &g_null_sampler);
		return g_null_sampler;
	}

	vk::image_view* null_image_view(const vk::command_buffer& cmd, VkImageViewType type)
	{
		if (static_cast<u32>(type) <= VK_IMAGE_VIEW_TYPE_2D_ARRAY && g_null_image_views[type])
		{
			return g_null_image_views[type]->get_view(rsx::default_remap_vector.with_encoding(VK_REMAP_IDENTITY));
		}

		VkImageType image_type;
		u32 num_layers = 1;
		u32 flags = 0;
		u16 size = 4;

		switch (type)
		{
		case VK_IMAGE_VIEW_TYPE_1D:
			image_type = VK_IMAGE_TYPE_1D;
			size = 1;
			break;
		case VK_IMAGE_VIEW_TYPE_2D:
			image_type = VK_IMAGE_TYPE_2D;
			break;
		case VK_IMAGE_VIEW_TYPE_3D:
			image_type = VK_IMAGE_TYPE_3D;
			break;
		case VK_IMAGE_VIEW_TYPE_CUBE:
			image_type = VK_IMAGE_TYPE_2D;
			flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
			num_layers = 6;
			break;
		case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
			image_type = VK_IMAGE_TYPE_2D;
			num_layers = 2;
			break;
		default:
			rsx_log.fatal("Unexpected image view type 0x%x", static_cast<u32>(type));
			return nullptr;
		}

		auto& tex = g_null_image_views[type];
		tex = std::make_unique<viewable_image>(*g_render_device, g_render_device->get_memory_mapping().device_local, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			image_type, VK_FORMAT_B8G8R8A8_UNORM, size, size, 1, 1, num_layers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, flags | VK_IMAGE_CREATE_ALLOW_NULL_RPCS3, VMM_ALLOCATION_POOL_SCRATCH);

		if (!tex->value)
		{
			// If we cannot create a 1x1 placeholder, things are truly hopeless.
			// The null view is 'nullable' because it is meant for use in emergency situations and we do not wish to invalidate any handles.
			fmt::throw_exception("Renderer is out of memory. We could not even squeeze in a 1x1 texture, things are bad.");
		}

		// Initialize memory to transparent black
		tex->change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		VkClearColorValue clear_color = {};
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, tex->mipmaps(), 0, tex->layers() };
		vkCmdClearColorImage(cmd, tex->value, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1, &range);

		// Prep for shader access
		tex->change_layout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		// Return view
		return tex->get_view(rsx::default_remap_vector.with_encoding(VK_REMAP_IDENTITY));
	}

	vk::image* get_typeless_helper(VkFormat format, rsx::format_class format_class, u32 requested_width, u32 requested_height)
	{
		auto create_texture = [&]()
		{
			u32 new_width = utils::align(requested_width, 256u);
			u32 new_height = utils::align(requested_height, 256u);

			return new vk::image(*g_render_device, g_render_device->get_memory_mapping().device_local, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				VK_IMAGE_TYPE_2D, format, new_width, new_height, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, VMM_ALLOCATION_POOL_SCRATCH,
				format_class);
		};

		const u64 key = (u64{format_class} << 32) | static_cast<u32>(format);
		auto ptr = static_cast<vk::image*>(key_list_find(&g_typeless_textures, key));

		if (ptr && ptr->width() >= requested_width && ptr->height() >= requested_height)
		{
			return ptr;
		}

		if (ptr)
		{
			requested_width = std::max(requested_width, ptr->width());
			requested_height = std::max(requested_height, ptr->height());
		}

		auto made = std::unique_ptr<vk::image>(create_texture());
		made->set_debug_name(fmt::format("Scratch: Format=0x%x", static_cast<u32>(format)));

		if (ptr)
		{
			// Grown: the new one in the old one's place, the old one to the GC
			std::unique_ptr<vk::image> old(static_cast<vk::image*>(key_list_replace(&g_typeless_textures, key, made.get())));
			get_resource_manager()->dispose(old);
		}
		else
		{
			ensure(key_list_add(&g_typeless_textures, key, made.get()));
		}

		return made.release();
	}

	std::pair<vk::buffer*, bool> get_scratch_buffer(u32 queue_family, u64 min_required_size)
	{
		ensure(queue_family < max_scratch_queue_families);
		auto& scratch_buffer = g_scratch_buffers_pool[queue_family].get_buf();
		bool is_new = false;

		if (scratch_buffer && scratch_buffer->size() < min_required_size)
		{
			// Scratch heap cannot fit requirements. Discard it and allocate a new one.
			vk::get_resource_manager()->dispose(scratch_buffer);
		}

		if (!scratch_buffer)
		{
			// Choose optimal size
			const u64 alloc_size = utils::align(min_required_size, 0x100000);

			scratch_buffer = std::make_unique<vk::buffer>(*g_render_device, alloc_size,
				g_render_device->get_memory_mapping().device_local, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, VMM_ALLOCATION_POOL_SCRATCH);

			is_new = true;
		}

		return { scratch_buffer.get(), is_new };
	}

	vk::buffer* get_scratch_buffer(const vk::command_buffer& cmd, u64 min_required_size, VkPipelineStageFlags dst_stage_flags, VkAccessFlags dst_access, bool zero_memory)
	{
		const auto [buf, init_mem] = get_scratch_buffer(cmd.get_queue_family(), min_required_size);

		if (init_mem || zero_memory)
		{
			// Zero-initialize the allocated VRAM
			const u64 zero_length = init_mem ? buf->size() : utils::align(min_required_size, 4);
			vkCmdFillBuffer(cmd, buf->value, 0, zero_length, 0);

			insert_buffer_memory_barrier(cmd, buf->value, 0, zero_length,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
		}
		else if (dst_access != VK_ACCESS_NONE)
		{
			insert_buffer_memory_barrier(cmd, buf->value, 0, min_required_size,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage_flags,
				VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, dst_access);
		}

		return buf;
	}

	void clear_scratch_resources()
	{
		for (auto& view : g_null_image_views)
		{
			view.reset();
		}

		for (auto& pool : g_scratch_buffers_pool)
		{
			pool = {};
		}

		for (u32 i = 0; i < g_typeless_textures.count; i++)
		{
			delete static_cast<vk::image*>(key_list_at(&g_typeless_textures, i));
		}

		key_list_free(&g_typeless_textures);

		if (g_null_sampler)
		{
			vkDestroySampler(*g_render_device, g_null_sampler, nullptr);
			g_null_sampler = nullptr;
		}
	}
}
