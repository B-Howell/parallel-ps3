#include "util/key_list.h"
#include "stdafx.h"

#include "VKResolveHelper.h"
#include "VKRenderPass.h"
#include "VKRenderTargets.h"

namespace
{
	const char *get_format_prefix(VkFormat format)
	{
		switch (format)
		{
			case VK_FORMAT_R5G6B5_UNORM_PACK16:
				return "r16";
			case VK_FORMAT_R8G8B8A8_UNORM:
			case VK_FORMAT_B8G8R8A8_UNORM:
				return "rgba8";
			case VK_FORMAT_R16G16B16A16_SFLOAT:
				return "rgba16f";
			case VK_FORMAT_R32G32B32A32_SFLOAT:
				return "rgba32f";
			case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
				return "r16";
			case VK_FORMAT_R8_UNORM:
				return "r8";
			case VK_FORMAT_R8G8_UNORM:
				return "rg8";
			case VK_FORMAT_R32_SFLOAT:
				return "r32f";
			default:
				fmt::throw_exception("Unhandled VkFormat 0x%x", u32(format));
		}
	}
}

namespace vk
{
	// By format, the few resolved: short lists (util/key_list.c), owned, where they were STL hash maps
	key_list_t g_resolve_helpers{};
	key_list_t g_unresolve_helpers{};
	std::unique_ptr<vk::depthonly_resolve> g_depth_resolver;
	std::unique_ptr<vk::depthonly_unresolve> g_depth_unresolver;
	std::unique_ptr<vk::stencilonly_resolve> g_stencil_resolver;
	std::unique_ptr<vk::stencilonly_unresolve> g_stencil_unresolver;
	std::unique_ptr<vk::depthstencil_resolve_EXT> g_depthstencil_resolver;
	std::unique_ptr<vk::depthstencil_unresolve_EXT> g_depthstencil_unresolver;

	template <typename T, typename ...Args>
	void initialize_pass(std::unique_ptr<T>& ptr, vk::render_device& dev, Args&&... extras)
	{
		if (!ptr)
		{
			ptr = std::make_unique<T>(std::forward<Args>(extras)...);
			ptr->create(dev);
		}
	}

	void resolve_image(vk::command_buffer& cmd, vk::viewable_image* dst, vk::viewable_image* src)
	{
		if (src->aspect() == VK_IMAGE_ASPECT_COLOR_BIT)
		{
			auto job = static_cast<vk::cs_resolve_task*>(key_list_find(&g_resolve_helpers, src->format()));

			if (!job)
			{
				const char* format_prefix = get_format_prefix(src->format());
				bool require_bgra_swap = false;

				if (vk::get_chip_family() == vk::chip_class::NV_kepler &&
					src->format() == VK_FORMAT_B8G8R8A8_UNORM)
				{
					// Workaround for NVIDIA kepler's broken image_load_store
					require_bgra_swap = true;
				}

				job = new vk::cs_resolve_task(format_prefix, require_bgra_swap);
				ensure(key_list_add(&g_resolve_helpers, src->format(), job));
			}

			job->run(cmd, src, dst);
		}
		else
		{
			vk::image* const surface[] = { dst };
			auto& dev = cmd.get_command_pool().get_owner();

			const auto key = vk::get_renderpass_key(surface);
			auto renderpass = vk::get_renderpass(dev, key);

			if (src->aspect() & VK_IMAGE_ASPECT_STENCIL_BIT)
			{
				if (dev.get_shader_stencil_export_support())
				{
					initialize_pass(g_depthstencil_resolver, dev);
					g_depthstencil_resolver->run(cmd, src, dst, renderpass);
				}
				else
				{
					initialize_pass(g_depth_resolver, dev);
					g_depth_resolver->run(cmd, src, dst, renderpass);

					// Chance for optimization here: If the stencil buffer was not used, simply perform a clear operation
					const auto stencil_init_flags = vk::as_rtt(src)->stencil_init_flags;
					if (stencil_init_flags & 0xFF00)
					{
						initialize_pass(g_stencil_resolver, dev);
						g_stencil_resolver->run(cmd, src, dst, renderpass);
					}
					else
					{
						VkClearDepthStencilValue clear{ 1.f, stencil_init_flags & 0xFF };
						VkImageSubresourceRange range{ VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };

						dst->push_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
						vkCmdClearDepthStencilImage(cmd, dst->value, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
						dst->pop_layout(cmd);
					}
				}
			}
			else
			{
				initialize_pass(g_depth_resolver, dev);
				g_depth_resolver->run(cmd, src, dst, renderpass);
			}
		}
	}

	void unresolve_image(vk::command_buffer& cmd, vk::viewable_image* dst, vk::viewable_image* src)
	{
		if (src->aspect() == VK_IMAGE_ASPECT_COLOR_BIT)
		{
			auto job = static_cast<vk::cs_unresolve_task*>(key_list_find(&g_unresolve_helpers, src->format()));

			if (!job)
			{
				const char* format_prefix = get_format_prefix(src->format());
				bool require_bgra_swap = false;

				if (vk::get_chip_family() == vk::chip_class::NV_kepler &&
					src->format() == VK_FORMAT_B8G8R8A8_UNORM)
				{
					// Workaround for NVIDIA kepler's broken image_load_store
					require_bgra_swap = true;
				}

				job = new vk::cs_unresolve_task(format_prefix, require_bgra_swap);
				ensure(key_list_add(&g_unresolve_helpers, src->format(), job));
			}

			job->run(cmd, dst, src);
		}
		else
		{
			vk::image* const surface[] = { dst };
			auto& dev = cmd.get_command_pool().get_owner();

			const auto key = vk::get_renderpass_key(surface);
			auto renderpass = vk::get_renderpass(dev, key);

			if (src->aspect() & VK_IMAGE_ASPECT_STENCIL_BIT)
			{
				if (dev.get_shader_stencil_export_support())
				{
					initialize_pass(g_depthstencil_unresolver, dev);
					g_depthstencil_unresolver->run(cmd, dst, src, renderpass);
				}
				else
				{
					initialize_pass(g_depth_unresolver, dev);
					g_depth_unresolver->run(cmd, dst, src, renderpass);

					// Chance for optimization here: If the stencil buffer was not used, simply perform a clear operation
					const auto stencil_init_flags = vk::as_rtt(dst)->stencil_init_flags;
					if (stencil_init_flags & 0xFF00)
					{
						initialize_pass(g_stencil_unresolver, dev);
						g_stencil_unresolver->run(cmd, dst, src, renderpass);
					}
					else
					{
						VkClearDepthStencilValue clear{ 1.f, stencil_init_flags & 0xFF };
						VkImageSubresourceRange range{ VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };

						dst->push_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
						vkCmdClearDepthStencilImage(cmd, dst->value, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
						dst->pop_layout(cmd);
					}
				}
			}
			else
			{
				initialize_pass(g_depth_unresolver, dev);
				g_depth_unresolver->run(cmd, dst, src, renderpass);
			}
		}
	}

	void clear_resolve_helpers()
	{
		for (u32 i = 0; i < g_resolve_helpers.count; i++)
		{
			auto task = static_cast<vk::cs_resolve_task*>(key_list_at(&g_resolve_helpers, i));
			task->destroy();
			delete task;
		}

		for (u32 i = 0; i < g_unresolve_helpers.count; i++)
		{
			auto task = static_cast<vk::cs_unresolve_task*>(key_list_at(&g_unresolve_helpers, i));
			task->destroy();
			delete task;
		}

		key_list_free(&g_resolve_helpers);
		key_list_free(&g_unresolve_helpers);

		if (g_depth_resolver)
		{
			g_depth_resolver->destroy();
			g_depth_resolver.reset();
		}

		if (g_stencil_resolver)
		{
			g_stencil_resolver->destroy();
			g_stencil_resolver.reset();
		}

		if (g_depthstencil_resolver)
		{
			g_depthstencil_resolver->destroy();
			g_depthstencil_resolver.reset();
		}

		if (g_depth_unresolver)
		{
			g_depth_unresolver->destroy();
			g_depth_unresolver.reset();
		}

		if (g_stencil_unresolver)
		{
			g_stencil_unresolver->destroy();
			g_stencil_unresolver.reset();
		}

		if (g_depthstencil_unresolver)
		{
			g_depthstencil_unresolver->destroy();
			g_depthstencil_unresolver.reset();
		}
	}

	void reset_resolve_resources()
	{
		if (g_depth_resolver) g_depth_resolver->free_resources();
		if (g_depth_unresolver) g_depth_unresolver->free_resources();
		if (g_stencil_resolver) g_stencil_resolver->free_resources();
		if (g_stencil_unresolver) g_stencil_unresolver->free_resources();
		if (g_depthstencil_resolver) g_depthstencil_resolver->free_resources();
		if (g_depthstencil_unresolver) g_depthstencil_unresolver->free_resources();
	}


	void cs_resolve_base::build(const std::string& format_prefix, bool unresolve, bool bgra_swap)
	{
		create();

		switch (optimal_group_size)
		{
		default:
		case 64:
			cs_wave_x = 8;
			cs_wave_y = 8;
			break;
		case 32:
			cs_wave_x = 8;
			cs_wave_y = 4;
			break;
		}

		static const char* resolve_kernel =
			#include "Emu/RSX/Program/MSAA/ColorResolvePass.glsl"
			;

		static const char* unresolve_kernel =
			#include "Emu/RSX/Program/MSAA/ColorUnresolvePass.glsl"
			;

		const std::pair<std::string_view, std::string> syntax_replace[] =
		{
			// The size is the specialization constants' (set below), so the
			// source is the same on every GPU
			{ "%WORKGROUP_SIZE_X", "1, local_size_x_id = 0" },
			{ "%WORKGROUP_SIZE_Y", "1, local_size_y_id = 1" },
			{ "%IMAGE_FORMAT", format_prefix },
			{ "%BGRA_SWAP", bgra_swap ? "1" : "0" }
		};

		m_src = unresolve ? unresolve_kernel : resolve_kernel;
		m_src = fmt::replace_all(m_src, syntax_replace);

		spec_values[0] = cs_wave_x;
		spec_values[1] = cs_wave_y;
		spec_count = 2;
	}

	void depth_resolve_base::build(bool resolve_depth, bool resolve_stencil, bool is_unresolve)
	{
		// Compiled when the core was built (vk_fixed_shaders.h)
		vs_fixed = VK_FIXED_passthrough_vs;

		if (resolve_depth && resolve_stencil)
		{
			fs_fixed = is_unresolve ? VK_FIXED_depth_stencil_unresolve_fs : VK_FIXED_depth_stencil_resolve_fs;
		}
		else if (resolve_depth)
		{
			fs_fixed = is_unresolve ? VK_FIXED_depth_unresolve_fs : VK_FIXED_depth_resolve_fs;
		}
		else if (resolve_stencil)
		{
			fs_fixed = is_unresolve ? VK_FIXED_stencil_unresolve_fs : VK_FIXED_stencil_resolve_fs;
		}
	}
}