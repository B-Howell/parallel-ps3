/* Pointers made once each into slots of their own, the slots kept
 * (rpcs3/util/slot_registry.c), where the Vulkan renderer's compute tasks
 * and overlay passes were an std::unordered_map by type index:
 *
 * - gone through in the order filled, an emptied slot skipped;
 * - cleared the last filled first, each slot emptied, so a use after makes
 *   it again; the registry usable again after;
 * - a full registry refuses more. */

#include "../../util/slot_registry.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

static slot_registry_t r;
static void *slots[SLOT_REGISTRY_MAX + 1];
static int objs[SLOT_REGISTRY_MAX + 1];
static int seen[16], n_seen;

static void note(void *p, void *ctx)
{
   (void)ctx;
   if (n_seen < 16)
      seen[n_seen] = (int)((int*)p - objs);
   n_seen++;
}

int main(void)
{
   int i;

   memset(&r, 0, sizeof(r));
   for (i = 0; i < 4; i++)
   {
      slots[i] = &objs[i];
      TEST_CHECK(slot_registry_add(&r, &slots[i]) == 1);
   }

   n_seen = 0;
   slot_registry_each(&r, note, NULL);
   TEST_CHECK(n_seen == 4 && seen[0] == 0 && seen[1] == 1 && seen[2] == 2 && seen[3] == 3);

   slots[1] = NULL;
   n_seen = 0;
   slot_registry_each(&r, note, NULL);
   TEST_CHECK(n_seen == 3 && seen[0] == 0 && seen[1] == 2 && seen[2] == 3);

   n_seen = 0;
   slot_registry_clear(&r, note, NULL);
   TEST_CHECK(n_seen == 3 && seen[0] == 3 && seen[1] == 2 && seen[2] == 0);
   TEST_CHECK(r.count == 0);
   for (i = 0; i < 4; i++)
      TEST_CHECK(slots[i] == NULL);

   /* Usable again, then full */
   for (i = 0; i < SLOT_REGISTRY_MAX; i++)
   {
      slots[i] = &objs[i];
      TEST_CHECK(slot_registry_add(&r, &slots[i]) == 1);
   }
   slots[SLOT_REGISTRY_MAX] = &objs[SLOT_REGISTRY_MAX];
   TEST_CHECK(slot_registry_add(&r, &slots[SLOT_REGISTRY_MAX]) == 0);
   n_seen = 0;
   slot_registry_clear(&r, note, NULL);
   TEST_CHECK(n_seen == SLOT_REGISTRY_MAX && r.count == 0 && slots[0] == NULL && slots[SLOT_REGISTRY_MAX - 1] == NULL);

   printf("test_slot_registry: slots made once each, gone through in order, cleared last first and emptied, full; %s\n", test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
