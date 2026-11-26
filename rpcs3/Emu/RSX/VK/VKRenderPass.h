#pragma once

#include <span>
#include "VulkanAPI.h"
#include "Utilities/geometry.h"

namespace vk
{
	class image;
	class command_buffer;

	// Spans: a one-image array or a slice of a constant list of ids, nothing made (they were vectors, built per draw)
	u64 get_renderpass_key(std::span<vk::image* const> images, std::span<const u8> input_attachment_ids = {});
	u64 get_renderpass_key(std::span<vk::image* const> images, u64 previous_key, std::span<const u8> input_attachment_ids = {});

	// 0, 1, 2 ...: the ids of the first n attachments, for programmable blending
	std::span<const u8> renderpass_attachment_ids(usz count);
	u64 get_renderpass_key(VkFormat surface_format, u8 color_attachment_count = 1, u8 sample_count = 1);
	u64 get_renderpass_key(VkFormat color_format, VkFormat depth_format, u8 color_attachment_count = 1, u8 sample_count = 1);
	VkRenderPass get_renderpass(VkDevice dev, u64 renderpass_key);

	bool renderpass_has_input_attachments(u64 renderpass_key);

	void clear_renderpass_cache(VkDevice dev);

	// Renderpass scope management helpers.
	// NOTE: These are not thread safe by design.
	void begin_renderpass(VkDevice dev, const vk::command_buffer& cmd, u64 renderpass_key, VkFramebuffer target, const coordu& framebuffer_region);
	void begin_renderpass(const vk::command_buffer& cmd, VkRenderPass pass, VkFramebuffer target, const coordu& framebuffer_region);
	void end_renderpass(const vk::command_buffer& cmd);
	bool is_renderpass_open(const vk::command_buffer& cmd);

	using renderpass_op_callback_t = std::function<void(const vk::command_buffer&, VkRenderPass, VkFramebuffer)>;
	void renderpass_op(const vk::command_buffer& cmd, const renderpass_op_callback_t& op);
}
