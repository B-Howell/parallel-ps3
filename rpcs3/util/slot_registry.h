#ifndef SLOT_REGISTRY_H__
#define SLOT_REGISTRY_H__

/* Pointers made once each, on first use, each kept in a slot of its own (a
 * static per type: one load to find), the slots kept here so all can be
 * gone through and emptied at once. It was a std::unordered_map by type
 * index, hashed on each use (the Vulkan renderer's compute tasks and
 * overlay passes, VKCompute.h, VKOverlays.h). The owner's thread alone.
 * C89. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLOT_REGISTRY_MAX 128

typedef struct slot_registry
{
   void   **slots[SLOT_REGISTRY_MAX];
   uint32_t count;
} slot_registry_t;

/* @slot, just filled, kept to be gone through; 0 if the registry is full */
int slot_registry_add(slot_registry_t *r, void **slot);

/* Each filled slot's pointer handed to @f, in the order they were filled */
void slot_registry_each(const slot_registry_t *r, void (*f)(void *p, void *ctx), void *ctx);

/* Each filled slot's pointer handed to @f, the last filled first, then the
 * slot emptied and the registry with it */
void slot_registry_clear(slot_registry_t *r, void (*f)(void *p, void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif

#endif
