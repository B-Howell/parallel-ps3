#include "stdafx.h"
#include "VulkanAPI.h"

#include "vkutils/device.h"

#define DECLARE_VK_FUNCTION_BODY
#include "VKProcTable.h"

namespace vk
{
	const render_device* get_current_renderer();

	bool load_vulkan()
	{
		return vk_loader_init() != 0;
	}

	void load_vulkan(PFN_vkGetInstanceProcAddr get_instance_proc_addr)
	{
		vk_loader_init_custom(get_instance_proc_addr);
	}

	void load_vulkan_instance(VkInstance instance)
	{
		vk_loader_load_instance(instance);
	}

	void init()
	{
		auto pdev = get_current_renderer();

		#define VK_FUNC(func) _##func = reinterpret_cast<PFN_##func>(vkGetDeviceProcAddr(*pdev, #func))
		#include "VKProcTable.h"
	}
}
