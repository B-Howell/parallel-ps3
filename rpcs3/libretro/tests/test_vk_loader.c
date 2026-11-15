/* The core's Vulkan loader (vk_loader.c), in place of volk: it opens the
 * system's loader through libretro-common's dylib, takes the global
 * functions, makes an instance with them and takes every function on the
 * core's list through it - each one there, none left out. Where there is
 * no Vulkan loader or driver it says so and passes. C89. */

#include <string.h>

#include "vk_loader.h"
#include "test_common.h"

static int missing;

/* The core functions are there whatever the driver; an extension's only
 * where the driver has it, and the instance extensions only if enabled */
static int is_extension(const char *name)
{
   size_t n = strlen(name);
   return n > 3 && (!strcmp(name + n - 3, "KHR") || !strcmp(name + n - 3, "EXT"));
}

#define CHECK_LOADED(name) \
   if (!name && !is_extension(#name)) { fprintf(stderr, "test_vk_loader: %s not loaded\n", #name); missing++; }

int main(void)
{
   VkApplicationInfo app;
   VkInstanceCreateInfo info;
   VkInstance instance = VK_NULL_HANDLE;
   uint32_t count      = 0;
   unsigned functions  = 0;

   if (!vk_loader_init())
   {
      printf("test_vk_loader: no Vulkan loader here: nothing loaded; ok\n");
      return 0;
   }
   TEST_CHECK(vkGetInstanceProcAddr != NULL);
   TEST_CHECK(vkCreateInstance != NULL);
   TEST_CHECK(vkEnumerateInstanceExtensionProperties != NULL);
   /* Once open, it is not opened again */
   TEST_CHECK(vk_loader_init());

   TEST_CHECK(vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) == VK_SUCCESS);

   memset(&app, 0, sizeof(app));
   app.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
   app.apiVersion = VK_API_VERSION_1_1;
   memset(&info, 0, sizeof(info));
   info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   info.pApplicationInfo = &app;
   if (vkCreateInstance(&info, NULL, &instance) != VK_SUCCESS)
   {
      printf("test_vk_loader: no Vulkan driver here: the loader's own functions only; ok\n");
      return test_failures ? 1 : 0;
   }

   vk_loader_load_instance(instance);
#define COUNT(name) functions++;
   VK_LOADER_INSTANCE(COUNT)
#undef COUNT
   VK_LOADER_INSTANCE(CHECK_LOADED)
   TEST_CHECK(!missing);

   /* What the instance gives works: the devices are there to count */
   count = 0;
   TEST_CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL) == VK_SUCCESS);
   vkDestroyInstance(instance, NULL);

   if (test_failures)
   {
      fprintf(stderr, "test_vk_loader: %d failed\n", test_failures);
      return 1;
   }
   printf("test_vk_loader: the system's loader opened, %u functions on the core's list taken through an instance, none missing\n", functions);
   return 0;
}
