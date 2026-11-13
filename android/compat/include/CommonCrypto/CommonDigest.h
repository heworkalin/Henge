/*
 * Android/Linux compatibility: <CommonCrypto/CommonDigest.h>
 *
 * devices/metallib.c reaches for CC_SHA256() to fingerprint a metallib so a
 * cached translation can be matched to its input. Implemented over the mbedtls
 * that is already vendored in third_party/mbedtls (3.6.2) rather than pulled in
 * as another dependency.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t CC_LONG;
typedef uint64_t CC_LONG64;

#define CC_SHA256_DIGEST_LENGTH 32
#define CC_SHA1_DIGEST_LENGTH 20
#define CC_MD5_DIGEST_LENGTH 16

/* One-shot SHA-256, mbedtls underneath. Returns md, as CommonCrypto does. */
unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md);

#ifdef __cplusplus
}
#endif
