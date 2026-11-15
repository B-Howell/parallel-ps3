/* The hashes rpcs3/Crypto's callers take from libretro-common in place of
 * the mbedtls/PolarSSL copies it had: MD5 (content and IRD checks) and
 * SHA-256 (cellGame's theme and file hashes, by sha256_get_hash), against
 * the published vectors, whole and fed in uneven pieces. C89. */

#include <lrc_hash.h>
#include "test_common.h"

static void hex(char *out, const uint8_t *d, size_t n)
{
   static const char pal[] = "0123456789abcdef";
   size_t i;
   for (i = 0; i < n; i++)
   {
      out[i * 2]     = pal[d[i] >> 4];
      out[i * 2 + 1] = pal[d[i] & 15];
   }
   out[n * 2] = '\0';
}

/* Fed in pieces of 1, 2, 3 ... bytes, so every block edge is crossed */
static void sha256_of(char *out, const uint8_t *d, size_t n, int pieces)
{
   struct sha256_state s;
   uint8_t digest[32];
   size_t at = 0, step = 1;

   sha256_stream_init(&s, 0);
   if (!pieces)
      sha256_stream_update(&s, d, n);
   else
      while (at < n)
      {
         size_t take = (n - at < step) ? n - at : step;
         sha256_stream_update(&s, d + at, take);
         at += take;
         step = step % 97 + 1;
      }
   sha256_stream_final(&s, digest);
   hex(out, digest, 32);
}

static void md5_of(char *out, const uint8_t *d, size_t n, int pieces)
{
   MD5_CTX c;
   unsigned char digest[16];
   size_t at = 0, step = 1;

   MD5_Init(&c);
   if (!pieces)
      MD5_Update(&c, d, (unsigned long)n);
   else
      while (at < n)
      {
         size_t take = (n - at < step) ? n - at : step;
         MD5_Update(&c, d + at, (unsigned long)take);
         at += take;
         step = step % 97 + 1;
      }
   MD5_Final(digest, &c);
   hex(out, digest, 16);
}

int main(void)
{
   static const char two_blocks[] =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
   static uint8_t million[1000000];
   char h[65];
   int pieces;

   memset(million, 'a', sizeof(million));

   for (pieces = 0; pieces < 2; pieces++)
   {
      /* FIPS 180-2 */
      sha256_of(h, (const uint8_t*)"", 0, pieces);
      TEST_CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
      sha256_of(h, (const uint8_t*)"abc", 3, pieces);
      TEST_CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
      sha256_of(h, (const uint8_t*)two_blocks, sizeof(two_blocks) - 1, pieces);
      TEST_CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
      sha256_of(h, million, sizeof(million), pieces);
      TEST_CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

      /* RFC 1321 */
      md5_of(h, (const uint8_t*)"", 0, pieces);
      TEST_CHECK(!strcmp(h, "d41d8cd98f00b204e9800998ecf8427e"));
      md5_of(h, (const uint8_t*)"abc", 3, pieces);
      TEST_CHECK(!strcmp(h, "900150983cd24fb0d6963f7d28e17f72"));
      md5_of(h, million, sizeof(million), pieces);
      TEST_CHECK(!strcmp(h, "7707d6ae4e027c70eea2a935c2296f21"));
   }

   if (test_failures)
   {
      fprintf(stderr, "test_crypto_hashes: %d failed\n", test_failures);
      return 1;
   }
   printf("test_crypto_hashes: MD5 and SHA-256 match the published vectors\n");
   return 0;
}
