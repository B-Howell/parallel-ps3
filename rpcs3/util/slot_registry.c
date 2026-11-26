/* Pointers made once each, kept in slots of their own (slot_registry.h). C89 */

#include "slot_registry.h"

int slot_registry_add(slot_registry_t *r, void **slot)
{
   if (r->count >= SLOT_REGISTRY_MAX)
      return 0;
   r->slots[r->count++] = slot;
   return 1;
}

void slot_registry_each(const slot_registry_t *r, void (*f)(void *p, void *ctx), void *ctx)
{
   uint32_t i;

   for (i = 0; i < r->count; i++)
      if (*r->slots[i])
         f(*r->slots[i], ctx);
}

void slot_registry_clear(slot_registry_t *r, void (*f)(void *p, void *ctx), void *ctx)
{
   while (r->count)
   {
      void **slot = r->slots[--r->count];

      if (*slot)
      {
         void *p = *slot;
         *slot   = 0;
         f(p, ctx);
      }
   }
}
