/* The Vulkan functions the core calls, loaded at run time (vk_loader.h).
 * C89. */

#include <stddef.h>

#include <dynamic/dylib.h>

#include "vk_loader.h"

#define VK_LOADER_DEFINE(name) PFN_##name name = NULL;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = NULL;
VK_LOADER_GLOBAL(VK_LOADER_DEFINE)
VK_LOADER_INSTANCE(VK_LOADER_DEFINE)
#undef VK_LOADER_DEFINE

/* Kept open for the life of the process, as the functions are */
static dylib_t vk_loader_lib = NULL;

static void vk_loader_load_global(void)
{
#define VK_LOADER_GET(name) name = (PFN_##name)vkGetInstanceProcAddr(NULL, #name);
   VK_LOADER_GLOBAL(VK_LOADER_GET)
#undef VK_LOADER_GET
}

int vk_loader_init(void)
{
   static const char *const names[] =
   {
#if defined(_WIN32)
      "vulkan-1.dll"
#elif defined(__APPLE__)
      "libvulkan.dylib",
      "libvulkan.1.dylib",
      "libMoltenVK.dylib"
#else
      "libvulkan.so.1",
      "libvulkan.so"
#endif
   };
   function_t sym = NULL;
   size_t i;

   if (vkGetInstanceProcAddr)
      return 1;

   for (i = 0; !vk_loader_lib && i < sizeof(names) / sizeof(names[0]); i++)
      vk_loader_lib = dylib_load(names[i]);
   if (vk_loader_lib)
      sym = dylib_proc(vk_loader_lib, "vkGetInstanceProcAddr");
   if (!sym)
      return 0;

   vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)sym;
   vk_loader_load_global();
   return 1;
}

void vk_loader_init_custom(PFN_vkGetInstanceProcAddr get_instance_proc_addr)
{
   vkGetInstanceProcAddr = get_instance_proc_addr;
   vk_loader_load_global();
}

void vk_loader_load_instance(VkInstance instance)
{
#define VK_LOADER_GET(name) name = (PFN_##name)vkGetInstanceProcAddr(instance, #name);
   VK_LOADER_INSTANCE(VK_LOADER_GET)
#undef VK_LOADER_GET
}
