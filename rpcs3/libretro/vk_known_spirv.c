/* The modules made ahead of time for GLSL the Vulkan renderer makes at run
 * time, found by the text (vk_known_spirv.h). C89. */

#include <string.h>

#include <lrc_hash.h>

#include "vk_known_spirv.h"

void vk_known_spirv_key(const char *text, size_t len, uint32_t stage, uint8_t key[16])
{
   struct sha256_state s;
   uint8_t             digest[32];
   uint8_t             st = (uint8_t)stage;
   sha256_stream_init(&s, 0);
   sha256_stream_update(&s, &st, 1);
   sha256_stream_update(&s, (const uint8_t*)text, len);
   sha256_stream_final(&s, digest);
   memcpy(key, digest, 16);
}

const struct vk_known_spirv *vk_known_spirv_find(const char *text, size_t len, uint32_t stage)
{
   uint8_t  key[16];
   uint32_t lo = 0;
   uint32_t hi = vk_known_spirv_table_count;
   if (!hi)
      return NULL;
   vk_known_spirv_key(text, len, stage, key);
   while (lo < hi)
   {
      const uint32_t               mid = lo + (hi - lo) / 2;
      const struct vk_known_spirv *k   = &vk_known_spirv_table[mid];
      const int                    c   = memcmp(key, k->key, 16);
      if (!c)
         return k->length == len && k->stage == stage ? k : NULL;
      if (c < 0)
         hi = mid;
      else
         lo = mid + 1;
   }
   return NULL;
}
