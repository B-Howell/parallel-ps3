// rpcs3/Crypto's AES on libretro-common's (crypto/crypto.h), through the
// core's own helpers and the shapes its callers use, against the published
// vectors:
//
// - aescbc128_decrypt/encrypt (EDAT, SDAT, cellSysmodule) on SP 800-38A
//   F.2.1/F.2.2, the IV given back zeroed, and a length that is not whole
//   blocks leaving the output as it was, as PolarSSL's did;
// - AES-256-CBC (the SELF metadata) on F.2.5/F.2.6;
// - aesecb128_encrypt on FIPS-197 C.1;
// - cmac_hash_compare/forge (EDAT, NPD) on RFC 4493's four lengths;
// - hmac_hash_compare/forge (EDAT) with the 16-byte keys they are given,
//   whose 20-byte MAC was written into a buffer of the key's length;
// - the PKG stream cipher: a big-endian be_t<u128> position as the counter
//   of aes_ctr128_crypt, against the block-by-block ECB it replaced, across
//   a carry out of the low 64 bits.
//
// Built by `make check` from the core's own objects and archives, the same
// link as the core's with this main. C++: the helpers and be_t are.

#include "stdafx.h"
#include "Crypto/utils.h"

#include <crypto/crypto.h>

#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static std::vector<u8> unhex(const char* s)
{
	std::vector<u8> out;
	for (; s[0] && s[1]; s += 2)
	{
		auto v = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
		out.push_back(static_cast<u8>(v(s[0]) << 4 | v(s[1])));
	}
	return out;
}

static const char* const F2_PT =
	"6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
	"30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";

static void cbc()
{
	const auto key = unhex("2b7e151628aed2a6abf7158809cf4f3c");
	const auto pt  = unhex(F2_PT);
	const auto ct  = unhex(
		"7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
		"73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");
	const auto iv0 = unhex("000102030405060708090a0b0c0d0e0f");
	std::vector<u8> out(64);
	u8 iv[16];

	std::memcpy(iv, iv0.data(), 16);
	aescbc128_encrypt(key.data(), iv, pt.data(), out.data(), 64);
	CHECK(out == ct);
	CHECK(std::memcmp(iv, std::vector<u8>(16).data(), 16) == 0);

	std::memcpy(iv, iv0.data(), 16);
	aescbc128_decrypt(key.data(), iv, ct.data(), out.data(), 64);
	CHECK(out == pt);

	// In place, as the EDAT blocks are
	std::memcpy(iv, iv0.data(), 16);
	out = ct;
	aescbc128_decrypt(key.data(), iv, out.data(), out.data(), 64);
	CHECK(out == pt);

	// Not whole blocks: nothing done
	std::memcpy(iv, iv0.data(), 16);
	std::vector<u8> untouched(64, 0x5a);
	out = untouched;
	aescbc128_decrypt(key.data(), iv, ct.data(), out.data(), 40);
	CHECK(out == untouched);
}

static void cbc256()
{
	const auto key = unhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
	const auto pt  = unhex(F2_PT);
	const auto ct  = unhex(
		"f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d"
		"39f23369a9d9bacfa530e26304231461b2eb05e2c39be9fcda6c19078c6a9d1b");
	auto iv = unhex("000102030405060708090a0b0c0d0e0f");
	std::vector<u8> out = ct;
	aes_ctx aes;

	CHECK(aes_init(&aes, key.data(), 32) == 0);
	aes_cbc_decrypt(&aes, iv.data(), out.data(), out.data(), out.size());
	CHECK(out == pt);
}

static void ecb()
{
	const auto key = unhex("000102030405060708090a0b0c0d0e0f");
	const auto pt  = unhex("00112233445566778899aabbccddeeff");
	const auto ct  = unhex("69c4e0d86a7b0430d8cdb78070b4c55a");
	std::vector<u8> out(16);
	aes_ctx aes;

	aesecb128_encrypt(key.data(), pt.data(), out.data());
	CHECK(out == ct);

	CHECK(aes_init(&aes, key.data(), 16) == 0);
	aes_decrypt_block(&aes, ct.data(), out.data());
	CHECK(out == pt);
}

static void cmac()
{
	const auto key = unhex("2b7e151628aed2a6abf7158809cf4f3c");
	const auto msg = unhex(F2_PT);
	static const struct { usz len; const char* mac; } v[] =
	{
		{ 0,  "bb1d6929e95937287fa37d129b756746" },
		{ 16, "070a16b46b4d4144f79bdd9dd04a287c" },
		{ 40, "dfa66747de9ae63030ca32611497c827" },
		{ 64, "51f0bebf7e3b9d92fc49741779363cfe" },
	};

	for (const auto& t : v)
	{
		const auto mac = unhex(t.mac);
		u8 forged[16];
		cmac_hash_forge(key.data(), 0x10, msg.data(), t.len, forged);
		CHECK(std::memcmp(forged, mac.data(), 16) == 0);
		CHECK(cmac_hash_compare(key.data(), 0x10, msg.data(), t.len, mac.data(), 0x10));

		auto wrong = mac;
		wrong[15] ^= 1;
		CHECK(!cmac_hash_compare(key.data(), 0x10, msg.data(), t.len, wrong.data(), 0x10));
	}
}

static void hmac()
{
	// A 16-byte key, as the EDAT hash keys are
	const auto key = unhex("000102030405060708090a0b0c0d0e0f");
	static const char msg[] = "what do ya want for nothing?";
	const auto mac = unhex("6a05036057f3095eca5df5fe3307949bf9d7c278");
	const u8* m = reinterpret_cast<const u8*>(msg);
	u8 forged[20];

	hmac_hash_forge(key.data(), 0x10, m, sizeof(msg) - 1, forged);
	CHECK(std::memcmp(forged, mac.data(), 20) == 0);

	// The first 16 bytes of the MAC under a 16-byte key, and all 20
	CHECK(hmac_hash_compare(key.data(), 0x10, m, sizeof(msg) - 1, mac.data(), 0x10));
	CHECK(hmac_hash_compare(key.data(), 0x10, m, sizeof(msg) - 1, mac.data(), 0x14));

	auto wrong = mac;
	wrong[3] ^= 1;
	CHECK(!hmac_hash_compare(key.data(), 0x10, m, sizeof(msg) - 1, wrong.data(), 0x10));
}

static void pkg_stream()
{
	const auto key = unhex("2b7e151628aed2a6abf7158809cf4f3c");
	aes_ctx aes;
	CHECK(aes_init(&aes, key.data(), 16) == 0);

	// Starts a few blocks before the low 64 bits wrap, so the carry runs into the high half
	const u128 start = (u128{0x0123456789abcdefull} << 64) | 0xfffffffffffffffcull;
	const usz blocks = 37;

	std::vector<u8> data(blocks * 16);
	for (usz i = 0; i < data.size(); i++)
		data[i] = static_cast<u8>(i * 7 + 3);

	// What unpkg did: the big-endian position through ECB a block at a time
	std::vector<u8> ref = data;
	be_t<u128> input = start;
	for (usz i = 0; i < blocks; i++, input++)
	{
		u8 ks[16];
		aes_encrypt_block(&aes, reinterpret_cast<const u8*>(&input), ks);
		for (usz j = 0; j < 16; j++)
			ref[i * 16 + j] ^= ks[j];
	}

	// What it does
	be_t<u128> ctr = start;
	aes_ctr128_crypt(&aes, reinterpret_cast<u8*>(&ctr), data.data(), data.data(), data.size());

	CHECK(data == ref);
	CHECK(ctr.value() == start + blocks);
}

int main()
{
	cbc();
	cbc256();
	ecb();
	cmac();
	hmac();
	pkg_stream();

	if (failures)
	{
		std::fprintf(stderr, "test_crypto_core: %d failed\n", failures);
		return 1;
	}

	std::printf("test_crypto_core: the core's AES-CBC, ECB, CMAC, HMAC and PKG stream match the published vectors\n");
	return 0;
}
