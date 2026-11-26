#ifndef BLOCK_BITS_H__
#define BLOCK_BITS_H__

/* A set of up to 4096 indices, a bit each, with a summary word holding a
 * bit for each 64 with any set: added and taken out with a store or two,
 * walked in rising order through only the words with a bit set. The
 * texture cache's blocks holding a section (Emu/RSX/Common/
 * texture_cache_utils.h), where an STL hash set of their pointers was kept.
 * The owner's thread alone. All inline. C89.
 *
 * Plain data: zeroed, a set is empty. */

#include <stdint.h>
#include <string.h>

#include <retro_inline.h>
#include <compat/intrinsics.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLOCK_BITS_MAX  4096
#define BLOCK_BITS_NONE 0xFFFFFFFFu

typedef struct block_bits
{
   uint64_t summary;                   /* bit w: words[w] has a bit set */
   uint64_t words[BLOCK_BITS_MAX / 64];
} block_bits_t;

static INLINE int block_bits_test(const block_bits_t *b, uint32_t i)
{
   return (int)(b->words[i / 64] >> (i & 63) & 1);
}

static INLINE void block_bits_set(block_bits_t *b, uint32_t i)
{
   b->words[i / 64] |= (uint64_t)1 << (i & 63);
   b->summary       |= (uint64_t)1 << (i / 64);
}

static INLINE void block_bits_clear(block_bits_t *b, uint32_t i)
{
   b->words[i / 64] &= ~((uint64_t)1 << (i & 63));
   if (!b->words[i / 64])
      b->summary &= ~((uint64_t)1 << (i / 64));
}

static INLINE void block_bits_clear_all(block_bits_t *b)
{
   memset(b, 0, sizeof(*b));
}

/* To walk the set in rising order: for each bit w of summary, each bit of
 * words[w] (a count of trailing zeros each, compat_ctz_u64), index
 * w * 64 + bit. Written out where it is walked, so the loop body is inlined
 * with it */

#ifdef __cplusplus
}
#endif

#endif
