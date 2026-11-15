/* The shader interpreter's pipelines with their fixed-function state made
 * dynamic, where the device can (VK_EXT_extended_dynamic_state 1 to 3).
 *
 * The interpreter runs any of the game's shaders, so what still sets one of
 * its pipelines apart from another is the fixed-function state: cull mode,
 * depth and stencil tests, blending, primitive type and the rest. Each
 * combination was a pipeline of its own, made the first time a draw needed
 * it - a hitch mid-game, since the interpreter's shaders are large - and
 * the warm-up at boot could only make a few dozen common ones.
 *
 * With that state dynamic, a pipeline stands for every combination of it.
 * vk_idyn_canonical() puts every dynamic field of a pipeline's state to one
 * value, so the pipelines are keyed and made by what is left (essentially
 * the render pass); vk_idyn_states() lists what the pipeline is to take as
 * dynamic; vk_idyn_record() sets the draw's real values on the command
 * buffer once the pipeline is bound.
 *
 * Fields that are dynamic in every interpreter pipeline already (stencil
 * masks and reference, depth bias factors, depth bounds, line width, blend
 * constants) are put to one value whatever the device has. */

#ifndef VK_INTERPRETER_DYNAMIC_H
#define VK_INTERPRETER_DYNAMIC_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the device can make dynamic */
#define VK_IDYN_BASE           0x01u /* EDS1 + EDS2: cull, front face, topology class, depth/stencil tests and ops, depth bias enable, primitive restart, rasterizer discard */
#define VK_IDYN_TOPOLOGY_ANY   0x02u /* any topology, not only its class (dynamicPrimitiveTopologyUnrestricted) */
#define VK_IDYN_LOGIC_OP       0x04u /* EDS2 logic op */
#define VK_IDYN_BLEND          0x08u /* EDS3 blend enable, blend equation, write mask */
#define VK_IDYN_LOGIC_OP_ON    0x10u /* EDS3 logic op enable */
#define VK_IDYN_DEPTH_CLAMP    0x20u /* EDS3 depth clamp enable */

/* The most extra dynamic states vk_idyn_states() gives */
#define VK_IDYN_MAX_STATES 20

struct vk_idyn_state
{
   VkPipelineInputAssemblyStateCreateInfo *ia;
   VkPipelineRasterizationStateCreateInfo *rs;
   VkPipelineDepthStencilStateCreateInfo  *ds;
   VkPipelineColorBlendStateCreateInfo    *cs;
   VkPipelineColorBlendAttachmentState    *att; /* cs->attachmentCount of them */
};

struct vk_idyn_fns
{
   PFN_vkCmdSetCullModeEXT               cull_mode;
   PFN_vkCmdSetFrontFaceEXT              front_face;
   PFN_vkCmdSetPrimitiveTopologyEXT      topology;
   PFN_vkCmdSetDepthTestEnableEXT        depth_test;
   PFN_vkCmdSetDepthWriteEnableEXT       depth_write;
   PFN_vkCmdSetDepthCompareOpEXT         depth_compare;
   PFN_vkCmdSetDepthBoundsTestEnableEXT  depth_bounds_test;
   PFN_vkCmdSetStencilTestEnableEXT      stencil_test;
   PFN_vkCmdSetStencilOpEXT              stencil_op;
   PFN_vkCmdSetDepthBiasEnableEXT        depth_bias;
   PFN_vkCmdSetPrimitiveRestartEnableEXT primitive_restart;
   PFN_vkCmdSetRasterizerDiscardEnableEXT rasterizer_discard;
   PFN_vkCmdSetLogicOpEXT                logic_op;
   PFN_vkCmdSetColorBlendEnableEXT       blend_enable;
   PFN_vkCmdSetColorBlendEquationEXT     blend_equation;
   PFN_vkCmdSetColorWriteMaskEXT         write_mask;
   PFN_vkCmdSetLogicOpEnableEXT          logic_op_enable;
   PFN_vkCmdSetDepthClampEnableEXT       depth_clamp;
};

/* caps as the device has them, given the functions it gave: a capability
 * whose functions are missing is dropped */
unsigned vk_idyn_usable(unsigned caps, const struct vk_idyn_fns *fns);

/* Puts every field the pipeline takes as dynamic to one value */
void vk_idyn_canonical(unsigned caps, const struct vk_idyn_state *s);

/* The dynamic states the pipeline takes beyond the ones every interpreter
 * pipeline has; out holds VK_IDYN_MAX_STATES. Gives how many. */
unsigned vk_idyn_states(unsigned caps, VkDynamicState *out);

/* Sets the draw's values of what is dynamic, the pipeline being bound */
void vk_idyn_record(const struct vk_idyn_fns *fns, VkCommandBuffer cmd,
      unsigned caps, const struct vk_idyn_state *s);

#ifdef __cplusplus
}
#endif

#endif
