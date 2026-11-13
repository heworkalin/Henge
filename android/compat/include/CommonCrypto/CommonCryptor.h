/*
 * Android/Linux compatibility: <CommonCrypto/CommonCryptor.h>
 *
 * devices/aes.c is the emulation of the vmapple AES engine, which the guest
 * drives for its own crypto. It used CCCrypt() for the actual cipher; here that
 * is mbedtls's AES (vendored in third_party/mbedtls 3.6.2).
 *
 * Only the one call shape the engine makes is implemented: AES-128/192/256 in
 * ECB or CBC, with PKCS#7 padding OFF. CCCrypt()'s default padding is PKCS#7,
 * which the engine switches off by passing 0 instead of kCCOptionPKCS7Padding;
 * a mode that asked for padding would need it spelled out here, and none does.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t CCCryptorStatus;
typedef uint32_t CCOperation;
typedef uint32_t CCAlgorithm;
typedef uint32_t CCOptions;

#define kCCSuccess 0
#define kCCParamError (-4300)
#define kCCBufferTooSmall (-4301)
#define kCCMemoryFailure (-4302)
#define kCCAlignmentError (-4303)
#define kCCDecodeError (-4304)
#define kCCUnimplemented (-4305)

#define kCCEncrypt 0
#define kCCDecrypt 1

#define kCCAlgorithmAES 0
#define kCCAlgorithmAES128 0

#define kCCOptionPKCS7Padding 0x0001
#define kCCOptionECBMode 0x0002

CCCryptorStatus CCCrypt(CCOperation op, CCAlgorithm alg, CCOptions options, const void *key, size_t keyLength,
                        const void *iv, const void *dataIn, size_t dataInLength, void *dataOut,
                        size_t dataOutAvailable, size_t *dataOutMoved);

#ifdef __cplusplus
}
#endif
