/*
 * Android/Linux compatibility: CommonCrypto, over the vendored mbedtls.
 *
 * Two calls are reached for:
 *   CC_SHA256()  devices/metallib.c — a metallib's fingerprint for the cache
 *   CCCrypt()    devices/aes.c      — the vmapple AES engine's cipher
 *
 * mbedtls 3.6.2's AES and SHA-256 are compiled in by the CMakeLists; nothing
 * here touches PSA, so the legacy low-level API is what is used.
 */
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>

#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>

#include <string.h>

/* ---- SHA-256 ---------------------------------------------------------- */

unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md) {
    if (!md) return NULL;
    if (mbedtls_sha256((const unsigned char *)data, (size_t)len, md, 0) != 0) return NULL;
    return md;
}

/* ---- AES -------------------------------------------------------------- */

/*
 * CBC in mbedtls takes a mutable IV and advances it, which is why the caller's
 * IV is copied. ECB has no IV at all. Neither path pads: CCCrypt's default is
 * PKCS#7, and the engine disables it, so a buffer whose length is not a whole
 * number of blocks is an alignment error rather than something to pad.
 */
CCCryptorStatus CCCrypt(CCOperation op, CCAlgorithm alg, CCOptions options, const void *key, size_t keyLength,
                        const void *iv, const void *dataIn, size_t dataInLength, void *dataOut,
                        size_t dataOutAvailable, size_t *dataOutMoved) {
    if (dataOutMoved) *dataOutMoved = 0;
    if (alg != kCCAlgorithmAES) return kCCUnimplemented;
    if (options & kCCOptionPKCS7Padding) return kCCUnimplemented; /* no caller wants it */
    if (!key || !dataIn || !dataOut) return kCCParamError;
    if (dataInLength % 16) return kCCAlignmentError;
    if (dataOutAvailable < dataInLength) return kCCBufferTooSmall;
    if (keyLength != 16 && keyLength != 24 && keyLength != 32) return kCCParamError;

    int enc = (op == kCCEncrypt);
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);

    int rc = enc ? mbedtls_aes_setkey_enc(&ctx, (const unsigned char *)key, (unsigned)(keyLength * 8))
                 : mbedtls_aes_setkey_dec(&ctx, (const unsigned char *)key, (unsigned)(keyLength * 8));
    if (rc != 0) {
        mbedtls_aes_free(&ctx);
        return kCCParamError;
    }

    if (options & kCCOptionECBMode) {
        const unsigned char *in = (const unsigned char *)dataIn;
        unsigned char *out = (unsigned char *)dataOut;
        for (size_t off = 0; off < dataInLength; off += 16) {
            rc = mbedtls_aes_crypt_ecb(&ctx, enc ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, in + off, out + off);
            if (rc != 0) break;
        }
    } else {
        if (!iv) {
            mbedtls_aes_free(&ctx);
            return kCCParamError;
        }
        unsigned char ivCopy[16];
        memcpy(ivCopy, iv, sizeof ivCopy);
        rc = mbedtls_aes_crypt_cbc(&ctx, enc ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, dataInLength, ivCopy,
                                   (const unsigned char *)dataIn, (unsigned char *)dataOut);
    }

    mbedtls_aes_free(&ctx);
    if (rc != 0) return kCCParamError;
    if (dataOutMoved) *dataOutMoved = dataInLength;
    return kCCSuccess;
}
