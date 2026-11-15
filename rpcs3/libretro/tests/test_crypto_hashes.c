/* The hashes rpcs3/Crypto's callers take from libretro-common in place of
 * the mbedtls/PolarSSL copies it had: MD5 (content and IRD checks),
 * SHA-256 (cellGame's theme and file hashes, by sha256_get_hash), SHA-1
 * (the PPU and SPU caches' hashes, the SELF/EDAT/TRP checks) and
 * HMAC-SHA1 (PUP, the SDAT/EDAT keys), against the published vectors,
 * whole and fed in uneven pieces. C89. */

#include <lrc_hash.h>
#include <crypto/kdf.h>
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

static void sha1_of(char *out, const uint8_t *d, size_t n, int pieces)
{
   struct sha1_state s;
   uint8_t digest[20];
   size_t at = 0, step = 1;

   if (!pieces)
      SHA1Digest(d, n, digest);
   else
   {
      sha1_stream_init(&s);
      while (at < n)
      {
         size_t take = (n - at < step) ? n - at : step;
         sha1_stream_update(&s, d + at, take);
         at += take;
         step = step % 97 + 1;
      }
      TEST_CHECK(sha1_stream_final(&s, digest));
   }
   hex(out, digest, 20);
}

static void hmac_sha1_of(char *out, const uint8_t *key, size_t key_len,
      const uint8_t *d, size_t n, int pieces)
{
   struct hmac_sha1_ctx c;
   uint8_t mac[20];
   size_t at = 0, step = 1;

   if (!pieces)
      hmac_sha1(key, key_len, d, n, mac);
   else
   {
      hmac_sha1_init(&c, key, key_len);
      while (at < n)
      {
         size_t take = (n - at < step) ? n - at : step;
         hmac_sha1_update(&c, d + at, take);
         at += take;
         step = step % 7 + 1;
      }
      hmac_sha1_final(&c, mac);
   }
   hex(out, mac, 20);
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
   static const char hi[] = "Hi There";
   static const char jefe_msg[] = "what do ya want for nothing?";
   static const char big_key_msg[] =
      "Test Using Larger Than Block-Size Key - Hash Key First";
   static uint8_t million[1000000];
   uint8_t key[80];
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

      /* FIPS 180-1 */
      sha1_of(h, (const uint8_t*)"", 0, pieces);
      TEST_CHECK(!strcmp(h, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
      sha1_of(h, (const uint8_t*)"abc", 3, pieces);
      TEST_CHECK(!strcmp(h, "a9993e364706816aba3e25717850c26c9cd0d89d"));
      sha1_of(h, (const uint8_t*)two_blocks, sizeof(two_blocks) - 1, pieces);
      TEST_CHECK(!strcmp(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
      sha1_of(h, million, sizeof(million), pieces);
      TEST_CHECK(!strcmp(h, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"));

      /* RFC 2202: a short key, a key shorter than the message, and a key
       * longer than a block, which is hashed first */
      memset(key, 0x0b, 20);
      hmac_sha1_of(h, key, 20, (const uint8_t*)hi, sizeof(hi) - 1, pieces);
      TEST_CHECK(!strcmp(h, "b617318655057264e28bc0b6fb378c8ef146be00"));
      hmac_sha1_of(h, (const uint8_t*)"Jefe", 4, (const uint8_t*)jefe_msg,
            sizeof(jefe_msg) - 1, pieces);
      TEST_CHECK(!strcmp(h, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
      memset(key, 0xaa, 80);
      hmac_sha1_of(h, key, 80, (const uint8_t*)big_key_msg,
            sizeof(big_key_msg) - 1, pieces);
      TEST_CHECK(!strcmp(h, "aa4ae5e15272d00e95705637ce8a3b55ed402112"));

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
   printf("test_crypto_hashes: MD5, SHA-1, HMAC-SHA1 and SHA-256 match the published vectors\n");
   return 0;
}
