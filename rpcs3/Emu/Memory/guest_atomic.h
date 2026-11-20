#ifndef GUEST_ATOMIC_H__
#define GUEST_ATOMIC_H__

/* Words of guest memory changed atomically by the host, as the PS3 has
 * them: big-endian, 4 or 8 bytes, aligned. A guest word is read and
 * written through retro_atomic, its value swapped on a little-endian host;
 * a read-modify-write that is not a plain swap loops on compare-and-swap of
 * the stored bytes. For what the emulated games keep in their own memory
 * (SPURS, cellSync, lightweight mutexes, RSX control words) and the HLE
 * functions change from the host. C89, header-only. */

#include <stdint.h>

#include <retro_inline.h>
#include <retro_atomic.h>

#if defined(_MSC_VER)
#include <stdlib.h>
#endif

#ifdef __cplusplus
extern "C" {
#define GUEST_PTR_(T, v) reinterpret_cast<T>(v)
#define GUEST_VAL_(T, v) static_cast<T>(v)
#else
#define GUEST_PTR_(T, v) ((T)(v))
#define GUEST_VAL_(T, v) ((T)(v))
#endif

#define GUEST_W32_(p) GUEST_PTR_(retro_atomic_int_t*, p)
#define GUEST_W64_(p) GUEST_PTR_(retro_atomic_64_t*, p)

/* Between the guest's byte order and the host's (the same swap both ways) */
static INLINE uint32_t guest_be32_(uint32_t v)
{
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
   return v;
#elif defined(_MSC_VER)
   return _byteswap_ulong(v);
#elif defined(__GNUC__)
   return __builtin_bswap32(v);
#else
   return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24);
#endif
}

static INLINE uint64_t guest_be64_(uint64_t v)
{
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
   return v;
#elif defined(_MSC_VER)
   return _byteswap_uint64(v);
#elif defined(__GNUC__)
   return __builtin_bswap64(v);
#else
   return (GUEST_VAL_(uint64_t, guest_be32_(GUEST_VAL_(uint32_t, v))) << 32) | guest_be32_(GUEST_VAL_(uint32_t, v >> 32));
#endif
}

/* ---- the bytes as they lie ----------------------------------------------
 * A word's bytes in the host's order, unswapped: for a structure of
 * big-endian fields copied whole */

static INLINE uint32_t guest_atomic_load_raw32(void *p)
{
   const int raw = retro_atomic_load_acquire_int(GUEST_W32_(p));
   return GUEST_VAL_(uint32_t, raw);
}

static INLINE void guest_atomic_store_raw32(void *p, uint32_t v)
{
   retro_atomic_store_release_int(GUEST_W32_(p), GUEST_VAL_(int, v));
}

static INLINE uint32_t guest_atomic_exchange_raw32(void *p, uint32_t v)
{
   const int raw = retro_atomic_exchange_int(GUEST_W32_(p), GUEST_VAL_(int, v));
   return GUEST_VAL_(uint32_t, raw);
}

static INLINE int guest_atomic_cas_raw32(void *p, uint32_t expected, uint32_t desired)
{
   return retro_atomic_cas_int(GUEST_W32_(p), GUEST_VAL_(int, expected), GUEST_VAL_(int, desired)) != 0;
}

static INLINE uint64_t guest_atomic_load_raw64(void *p)
{
   const int64_t raw = retro_atomic_load_acquire_64(GUEST_W64_(p));
   return GUEST_VAL_(uint64_t, raw);
}

static INLINE void guest_atomic_store_raw64(void *p, uint64_t v)
{
   retro_atomic_store_release_64(GUEST_W64_(p), GUEST_VAL_(int64_t, v));
}

static INLINE uint64_t guest_atomic_exchange_raw64(void *p, uint64_t v)
{
   const int64_t raw = retro_atomic_exchange_64(GUEST_W64_(p), GUEST_VAL_(int64_t, v));
   return GUEST_VAL_(uint64_t, raw);
}

static INLINE int guest_atomic_cas_raw64(void *p, uint64_t expected, uint64_t desired)
{
   return retro_atomic_cas_64(GUEST_W64_(p), GUEST_VAL_(int64_t, expected), GUEST_VAL_(int64_t, desired)) != 0;
}

/* ---- 32-bit ---------------------------------------------------------- */

static INLINE uint32_t guest_atomic_load_be32(void *p)
{
   const int raw = retro_atomic_load_acquire_int(GUEST_W32_(p));
   return guest_be32_(GUEST_VAL_(uint32_t, raw));
}

static INLINE void guest_atomic_store_be32(void *p, uint32_t v)
{
   retro_atomic_store_release_int(GUEST_W32_(p), GUEST_VAL_(int, guest_be32_(v)));
}

/* The value that was there */
static INLINE uint32_t guest_atomic_exchange_be32(void *p, uint32_t v)
{
   const int raw = retro_atomic_exchange_int(GUEST_W32_(p), GUEST_VAL_(int, guest_be32_(v)));
   return guest_be32_(GUEST_VAL_(uint32_t, raw));
}

/* Nonzero if it was @expected and is @desired now */
static INLINE int guest_atomic_cas_be32(void *p, uint32_t expected, uint32_t desired)
{
   return retro_atomic_cas_int(GUEST_W32_(p), GUEST_VAL_(int, guest_be32_(expected)),
         GUEST_VAL_(int, guest_be32_(desired))) != 0;
}

/* The value before adding @v (wrapping) */
static INLINE uint32_t guest_atomic_fetch_add_be32(void *p, uint32_t v)
{
   uint32_t old = guest_atomic_load_be32(p);
   while (!guest_atomic_cas_be32(p, old, old + v))
      old = guest_atomic_load_be32(p);
   return old;
}

/* ---- 64-bit ---------------------------------------------------------- */

static INLINE uint64_t guest_atomic_load_be64(void *p)
{
   const int64_t raw = retro_atomic_load_acquire_64(GUEST_W64_(p));
   return guest_be64_(GUEST_VAL_(uint64_t, raw));
}

static INLINE void guest_atomic_store_be64(void *p, uint64_t v)
{
   retro_atomic_store_release_64(GUEST_W64_(p), GUEST_VAL_(int64_t, guest_be64_(v)));
}

static INLINE uint64_t guest_atomic_exchange_be64(void *p, uint64_t v)
{
   const int64_t raw = retro_atomic_exchange_64(GUEST_W64_(p), GUEST_VAL_(int64_t, guest_be64_(v)));
   return guest_be64_(GUEST_VAL_(uint64_t, raw));
}

static INLINE int guest_atomic_cas_be64(void *p, uint64_t expected, uint64_t desired)
{
   return retro_atomic_cas_64(GUEST_W64_(p), GUEST_VAL_(int64_t, guest_be64_(expected)),
         GUEST_VAL_(int64_t, guest_be64_(desired))) != 0;
}

/* The value before adding @v (wrapping; a subtraction adds the negation) */
static INLINE uint64_t guest_atomic_fetch_add_be64(void *p, uint64_t v)
{
   uint64_t old = guest_atomic_load_be64(p);
   while (!guest_atomic_cas_be64(p, old, old + v))
      old = guest_atomic_load_be64(p);
   return old;
}

#ifdef __cplusplus
}
#endif

#endif
