/* The Vulkan renderer's fixed shaders, compiled once to SPIR-V and kept in
 * the core (vk_fixed_spirv.c): nothing compiles them while a game runs.
 * C89.
 *
 * The list below is what vk_fixed_spirv.c is made from. Each shader is a
 * GLSL file of the renderer's own (a C++ raw string, R"( ... )", as the GL
 * renderer still includes them), with the same words put in for the same
 * markers the renderer put in when it compiled the source itself: each
 * marker in turn, at each place, the first in the list that matches there.
 *
 * vk_fixed_spirv.c is generated: after a change to a shader here or to
 * the compiler, `make vk-fixed-shaders` writes it again, and `make check`
 * fails while it is not what they make. */

#ifndef VK_FIXED_SHADERS_H
#define VK_FIXED_SHADERS_H

#include <stdint.h>

#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* The stages, as rslc's (enum rslc_stage) */
#define VK_FIXED_VERTEX   0
#define VK_FIXED_FRAGMENT 1
#define VK_FIXED_COMPUTE  3

/* X(name, stage, file under rpcs3/, markers): markers are "marker\0words\0"
 * pairs, "" for none */
#define VK_FIXED_SHADERS(X) \
   X(overlay_vs, VK_FIXED_VERTEX, "Emu/RSX/Program/GLSLSnippets/OverlayRenderVS.glsl", \
         "%preprocessor\0// %preprocessor\0" "%push_block\0push_constant\0") \
   X(overlay_fs, VK_FIXED_FRAGMENT, "Emu/RSX/Program/GLSLSnippets/OverlayRenderFS.glsl", \
         "%preprocessor\0// %preprocessor\0" "%push_block_offset\0layout(offset=68)\0" \
         "%push_block\0push_constant\0") \
   X(attachment_clear_vs, VK_FIXED_VERTEX, "Emu/RSX/Program/GLSLSnippets/AttachmentClearVS.glsl", "") \
   X(attachment_clear_fs, VK_FIXED_FRAGMENT, "Emu/RSX/Program/GLSLSnippets/AttachmentClearFS.glsl", "") \
   X(stencil_clear_vs, VK_FIXED_VERTEX, "Emu/RSX/Program/GLSLSnippets/StencilClearVS.glsl", "") \
   X(stencil_clear_fs, VK_FIXED_FRAGMENT, "Emu/RSX/Program/GLSLSnippets/StencilClearFS.glsl", "") \
   X(passthrough_vs, VK_FIXED_VERTEX, "Emu/RSX/Program/GLSLSnippets/GenericVSPassthrough.glsl", "") \
   X(video_out_calibration_fs, VK_FIXED_FRAGMENT, "Emu/RSX/Program/GLSLSnippets/VideoOutCalibrationPass.glsl", \
         "%sampler_binding\0x\0" "%set_decorator\0set=0\0") \
   X(depth_resolve_fs, VK_FIXED_FRAGMENT, "Emu/RSX/Program/MSAA/DepthResolvePass.glsl", "") \
   X(depth_unresolve_fs, VK_FIXED_FRAGMENT, "Emu/RSX/Program/MSAA/DepthUnresolvePass.glsl", "") \
   X(stencil_resolve_fs, VK_FIXED_FRAGMENT, "Emu/RSX/Program/MSAA/StencilResolvePass.glsl", "") \
   X(stencil_unresolve_fs, VK_FIXED_FRAGMENT, "Emu/RSX/Program/MSAA/StencilUnresolvePass.glsl", "") \
   X(depth_stencil_resolve_fs, VK_FIXED_FRAGMENT, "Emu/RSX/Program/MSAA/DepthStencilResolvePass.glsl", "") \
   X(depth_stencil_unresolve_fs, VK_FIXED_FRAGMENT, "Emu/RSX/Program/MSAA/DepthStencilUnresolvePass.glsl", "")

#define VK_FIXED_ENUM(name, stage, file, markers) VK_FIXED_##name,
enum vk_fixed_shader
{
   VK_FIXED_SHADERS(VK_FIXED_ENUM)
   VK_FIXED_COUNT
};
#undef VK_FIXED_ENUM

/* One shader's module, in the core's own memory: lent to the driver as it
 * is, never copied */
struct vk_fixed_spirv
{
   const uint32_t *words;
   uint32_t        count;
   uint32_t        stage;   /* VK_FIXED_* */
};

extern const struct vk_fixed_spirv vk_fixed_spirv_table[VK_FIXED_COUNT];

RETRO_END_DECLS

#endif
