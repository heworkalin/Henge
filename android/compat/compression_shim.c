/*
 * libcompression's decode entry point, on top of Apple's open-source lzfse.
 *
 * Only LZFSE is implemented, because that is what the engine asks for. An LZFSE stream carries
 * LZVN blocks inside it, and the decoder handles those itself, so one implementation covers the
 * compressed-file forks in an APFS volume as well.
 *
 * Returning 0 for an unimplemented algorithm is deliberate: every caller treats 0 as "this chunk
 * cannot be decoded" and fails the surrounding operation, which is the right outcome — silently
 * returning a short buffer would hand back corrupt bytes as if they were good.
 */
#include <Compression.h>

#include "lzfse.h"

size_t compression_decode_buffer(uint8_t *dst, size_t dst_size,
                                 const uint8_t *src, size_t src_size,
                                 void *scratch, compression_algorithm algorithm) {
    (void)scratch;
    if (!dst || !src) return 0;

    switch (algorithm) {
    case COMPRESSION_LZFSE: {
        size_t got = lzfse_decode_buffer(dst, dst_size, src, src_size, NULL);
        return got;
    }
    default:
        return 0;
    }
}
