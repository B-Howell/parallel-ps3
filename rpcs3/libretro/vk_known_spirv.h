/* Modules compiled when the core was built for GLSL the Vulkan renderer
 * makes at run time: what its compute tasks write, from their own code,
 * is the same text on every GPU (what differs between GPUs is left to
 * specialization constants), so the module for it was made ahead of time
 * and is found here by the text instead of being compiled. C89.
 *
 * Found by the text's SHA-256 (the first 128 bits) and its length, with
 * its stage: a text not here is compiled as before. The table
 * (vk_known_spirv_data.c) is written by the shader corpus test, which
 * makes every one of these texts through the renderer's own code
 * (SHADER_CORPUS_KNOWN=<file>), and the test fails while the table is not
 * what they make now. */

#ifndef VK_KNOWN_SPIRV_H
#define VK_KNOWN_SPIRV_H

#include <stddef.h>
#include <stdint.h>

#include <retro_common_api.h>

RETRO_BEGIN_DECLS

struct vk_known_spirv
{
   uint8_t         key[16];   /* the text's SHA-256 after its stage's byte */
   uint32_t        length;    /* the text's */
   uint32_t        stage;     /* as vk_fixed_shaders.h's VK_FIXED_* */
   const uint32_t *words;
   uint32_t        count;
};

/* Sorted by key */
extern const struct vk_known_spirv vk_known_spirv_table[];
extern const uint32_t              vk_known_spirv_table_count;

/* The key of @len bytes of @text for @stage */
void vk_known_spirv_key(const char *text, size_t len, uint32_t stage, uint8_t key[16]);

/* The module made for the text, lent from the core's memory, or NULL */
const struct vk_known_spirv *vk_known_spirv_find(const char *text, size_t len, uint32_t stage);

RETRO_END_DECLS

#endif
