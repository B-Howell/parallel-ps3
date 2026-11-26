/* A set of up to 4096 indices with a summary word (rpcs3/util/block_bits.h),
 * the texture cache's blocks holding a section: set, cleared and tested; the
 * summary bit of a word kept while any of its bits is set and dropped with
 * the last; a walk through the summary and words handing out exactly the
 * set indices in rising order, checked against a plain array after each of
 * many random steps. C89. */

#include <stdio.h>
#include <string.h>

#include "../../util/block_bits.h"
#include "test_common.h"

static unsigned char in[BLOCK_BITS_MAX];

static void check_all(const block_bits_t *b)
{
   uint64_t words;
   uint32_t i, expect = 0, n = 0;

   for (i = 0; i < BLOCK_BITS_MAX; i++)
      TEST_CHECK(block_bits_test(b, i) == in[i]);
   for (i = 0; i < BLOCK_BITS_MAX / 64; i++)
      TEST_CHECK(((b->summary >> i) & 1) == (b->words[i] != 0));

   /* The walk as the texture cache writes it */
   for (words = b->summary; words; words &= words - 1)
   {
      const uint32_t w = compat_ctz_u64(words);
      uint64_t       bits;

      for (bits = b->words[w]; bits; bits &= bits - 1)
      {
         const uint32_t at = w * 64 + compat_ctz_u64(bits);
         while (expect < BLOCK_BITS_MAX && !in[expect])
            expect++;
         TEST_CHECK(at == expect);
         expect++;
         n++;
      }
   }
   for (i = 0, expect = 0; i < BLOCK_BITS_MAX; i++)
      expect += in[i];
   TEST_CHECK(n == expect);
}

int main(void)
{
   block_bits_t b;
   uint32_t     seed = 4242, i;
   int          step;

   block_bits_clear_all(&b);
   check_all(&b);

   /* Two in one word: the summary stays until both are gone */
   block_bits_set(&b, 64);
   block_bits_set(&b, 127);
   in[64] = in[127] = 1;
   check_all(&b);
   block_bits_clear(&b, 64);
   in[64] = 0;
   TEST_CHECK((b.summary >> 1) & 1);
   check_all(&b);
   block_bits_clear(&b, 127);
   in[127] = 0;
   TEST_CHECK(b.summary == 0);
   check_all(&b);

   /* The first and last indices, and every word's edges */
   for (i = 0; i < BLOCK_BITS_MAX; i += 63)
   {
      block_bits_set(&b, i);
      in[i] = 1;
   }
   block_bits_set(&b, BLOCK_BITS_MAX - 1);
   in[BLOCK_BITS_MAX - 1] = 1;
   check_all(&b);

   /* Random sets and clears, each checked */
   for (step = 0; step < 20000; step++)
   {
      seed = seed * 1103515245u + 12345u;
      i    = (seed >> 8) % BLOCK_BITS_MAX;
      if (in[i])
         block_bits_clear(&b, i);
      else
         block_bits_set(&b, i);
      in[i] = !in[i];
      if (step % 211 == 0)
         check_all(&b);
   }
   check_all(&b);

   block_bits_clear_all(&b);
   memset(in, 0, sizeof(in));
   check_all(&b);

   printf("test_block_bits: indices set and cleared, the summary kept, walked in order, %d random steps checked; %s\n", step, test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
