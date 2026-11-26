#ifndef VK_SAMPLER_TABLE_H__
#define VK_SAMPLER_TABLE_H__

/* The Vulkan renderer's samplers by their key (vkutils/sampler.cpp): a
 * fixed table, made once with the renderer and never grown, looked up on
 * each texture bind that changes its sampler. It was two
 * std::unordered_maps, hashed and grown on each new sampler.
 *
 * Open-addressed with linear probing; a sampler taken out has the run
 * after it shifted back, so no slot is left marked deleted. The resource
 * manager trims the samplers no texture holds once there are more than
 * 2048 (VKResourceManager.cpp), so the table holds four times that. C89. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VK_SAMPLER_SLOTS 8192 /* a power of two */

typedef struct vk_sampler_table
{
   uint64_t base[VK_SAMPLER_SLOTS];
   uint64_t border[VK_SAMPLER_SLOTS];
   void    *obj[VK_SAMPLER_SLOTS];   /* NULL: empty */
   uint32_t count;
} vk_sampler_table_t;

void *vk_sampler_table_find(const vk_sampler_table_t *t, uint64_t base, uint64_t border);

/* 1: kept; 0: the table full, or the key there already (not replaced) */
int vk_sampler_table_insert(vk_sampler_table_t *t, uint64_t base, uint64_t border, void *obj);

/* Each sampler @pick says so of is taken out and handed to @take; with no
 * @pick, all of them */
void vk_sampler_table_collect(vk_sampler_table_t *t, int (*pick)(void *obj, void *ctx), void (*take)(void *obj, void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif

#endif
