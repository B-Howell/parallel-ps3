/* The Vulkan functions the core calls, loaded at run time: from the
 * frontend's vkGetInstanceProcAddr (the libretro Vulkan interface), or
 * from the system's loader opened with libretro-common's dylib
 * (vulkan-1.dll, libvulkan.so.1, MoltenVK). The core links no Vulkan
 * library. C89.
 *
 * In place of volk. The core's own list: what it calls and nothing else,
 * each a pointer named as the function, so the code calls vkCmdDraw(...)
 * as it would the prototype (VK_NO_PROTOTYPES leaves the names free).
 * Instance and device functions alike are taken through the instance, as
 * volk's volkLoadInstance took them; the device extensions the renderer
 * asks for are its own (VKProcTable.h). Written when an instance is made
 * or adopted, before any thread calls them. */

#ifndef VK_LOADER_H
#define VK_LOADER_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Taken with no instance */
#define VK_LOADER_GLOBAL(X) \
   X(vkCreateInstance) \
   X(vkEnumerateInstanceExtensionProperties)

/* Taken through the instance */
#define VK_LOADER_INSTANCE(X) \
   X(vkAcquireNextImageKHR) \
   X(vkAllocateCommandBuffers) \
   X(vkAllocateDescriptorSets) \
   X(vkAllocateMemory) \
   X(vkBeginCommandBuffer) \
   X(vkBindBufferMemory) \
   X(vkBindImageMemory) \
   X(vkCmdBeginQuery) \
   X(vkCmdBeginRenderPass) \
   X(vkCmdBeginTransformFeedbackEXT) \
   X(vkCmdBindDescriptorSets) \
   X(vkCmdBindIndexBuffer) \
   X(vkCmdBindPipeline) \
   X(vkCmdBindTransformFeedbackBuffersEXT) \
   X(vkCmdBindVertexBuffers) \
   X(vkCmdBlitImage) \
   X(vkCmdClearAttachments) \
   X(vkCmdClearColorImage) \
   X(vkCmdClearDepthStencilImage) \
   X(vkCmdCopyBuffer) \
   X(vkCmdCopyBufferToImage) \
   X(vkCmdCopyImage) \
   X(vkCmdCopyImageToBuffer) \
   X(vkCmdCopyQueryPoolResults) \
   X(vkCmdDispatch) \
   X(vkCmdDraw) \
   X(vkCmdDrawIndexed) \
   X(vkCmdEndQuery) \
   X(vkCmdEndRenderPass) \
   X(vkCmdEndTransformFeedbackEXT) \
   X(vkCmdFillBuffer) \
   X(vkCmdPipelineBarrier) \
   X(vkCmdPushConstants) \
   X(vkCmdResetQueryPool) \
   X(vkCmdSetBlendConstants) \
   X(vkCmdSetDepthBias) \
   X(vkCmdSetDepthBounds) \
   X(vkCmdSetEvent) \
   X(vkCmdSetLineWidth) \
   X(vkCmdSetScissor) \
   X(vkCmdSetStencilCompareMask) \
   X(vkCmdSetStencilReference) \
   X(vkCmdSetStencilWriteMask) \
   X(vkCmdSetViewport) \
   X(vkCmdUpdateBuffer) \
   X(vkCmdWaitEvents) \
   X(vkCreateBuffer) \
   X(vkCreateBufferView) \
   X(vkCreateCommandPool) \
   X(vkCreateComputePipelines) \
   X(vkCreateDebugReportCallbackEXT) \
   X(vkCreateDescriptorPool) \
   X(vkCreateDescriptorSetLayout) \
   X(vkCreateDevice) \
   X(vkCreateEvent) \
   X(vkCreateFence) \
   X(vkCreateFramebuffer) \
   X(vkCreateGraphicsPipelines) \
   X(vkCreateImage) \
   X(vkCreateImageView) \
   X(vkCreatePipelineCache) \
   X(vkCreatePipelineLayout) \
   X(vkCreateQueryPool) \
   X(vkCreateRenderPass) \
   X(vkCreateSampler) \
   X(vkCreateSemaphore) \
   X(vkCreateShaderModule) \
   X(vkCreateSwapchainKHR) \
   X(vkDestroyBuffer) \
   X(vkDestroyBufferView) \
   X(vkDestroyCommandPool) \
   X(vkDestroyDebugReportCallbackEXT) \
   X(vkDestroyDescriptorPool) \
   X(vkDestroyDescriptorSetLayout) \
   X(vkDestroyDevice) \
   X(vkDestroyEvent) \
   X(vkDestroyFence) \
   X(vkDestroyFramebuffer) \
   X(vkDestroyImage) \
   X(vkDestroyImageView) \
   X(vkDestroyInstance) \
   X(vkDestroyPipeline) \
   X(vkDestroyPipelineCache) \
   X(vkDestroyPipelineLayout) \
   X(vkDestroyQueryPool) \
   X(vkDestroyRenderPass) \
   X(vkDestroySampler) \
   X(vkDestroySemaphore) \
   X(vkDestroyShaderModule) \
   X(vkDestroySurfaceKHR) \
   X(vkDestroySwapchainKHR) \
   X(vkDeviceWaitIdle) \
   X(vkEndCommandBuffer) \
   X(vkEnumerateDeviceExtensionProperties) \
   X(vkEnumeratePhysicalDevices) \
   X(vkFreeCommandBuffers) \
   X(vkFreeMemory) \
   X(vkGetBufferMemoryRequirements) \
   X(vkGetDeviceProcAddr) \
   X(vkGetDeviceQueue) \
   X(vkGetEventStatus) \
   X(vkGetFenceStatus) \
   X(vkGetPipelineCacheData) \
   X(vkGetImageMemoryRequirements) \
   X(vkGetImageSubresourceLayout) \
   X(vkGetPhysicalDeviceFeatures) \
   X(vkGetPhysicalDeviceFeatures2) \
   X(vkGetPhysicalDeviceFormatProperties) \
   X(vkGetPhysicalDeviceMemoryProperties) \
   X(vkGetPhysicalDeviceProperties) \
   X(vkGetPhysicalDeviceProperties2) \
   X(vkGetPhysicalDeviceQueueFamilyProperties) \
   X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
   X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
   X(vkGetPhysicalDeviceSurfaceSupportKHR) \
   X(vkGetQueryPoolResults) \
   X(vkGetSwapchainImagesKHR) \
   X(vkMapMemory) \
   X(vkMergePipelineCaches) \
   X(vkQueuePresentKHR) \
   X(vkQueueSubmit) \
   X(vkQueueWaitIdle) \
   X(vkResetCommandBuffer) \
   X(vkResetDescriptorPool) \
   X(vkResetEvent) \
   X(vkResetFences) \
   X(vkSetEvent) \
   X(vkUnmapMemory) \
   X(vkUpdateDescriptorSets) \
   X(vkWaitForFences)

#define VK_LOADER_DECLARE(name) extern PFN_##name name;
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
VK_LOADER_GLOBAL(VK_LOADER_DECLARE)
VK_LOADER_INSTANCE(VK_LOADER_DECLARE)
#undef VK_LOADER_DECLARE

/* Opens the system's loader, once. Returns 0 when there is none, or it
 * has no vkGetInstanceProcAddr */
int vk_loader_init(void);

/* The frontend's vkGetInstanceProcAddr in place of the system's */
void vk_loader_init_custom(PFN_vkGetInstanceProcAddr get_instance_proc_addr);

/* Every instance and device function, through instance */
void vk_loader_load_instance(VkInstance instance);

#ifdef __cplusplus
}
#endif

#endif
