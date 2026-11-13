/*
 * Apple's Compression framework, narrowed to what this port actually calls.
 *
 * The engine's DMG and IPSW readers were written against libcompression, which iOS has and Android
 * does not. lzfse is open source and decodes the same format; this header is the seam, so those
 * readers keep their shape instead of growing an #ifdef per call site.
 */
#ifndef HENGE_COMPAT_COMPRESSION_H
#define HENGE_COMPAT_COMPRESSION_H

#include <stddef.h>
#include <stdint.h>

/* The values Apple uses; code passes them by name, but they are kept equal anyway. */
typedef enum {
    COMPRESSION_LZ4 = 0x100,
    COMPRESSION_ZLIB = 0x200,
    COMPRESSION_LZMA = 0x300,
    COMPRESSION_LZFSE = 0x700,
} compression_algorithm;

/*
 * Decompresses `src` into `dst`. Returns the number of bytes written, or 0 when the input is not
 * the format asked for, does not fit, or the algorithm is one this port does not implement.
 *
 * `scratch` is accepted and ignored: lzfse needs no caller-provided workspace.
 */
size_t compression_decode_buffer(uint8_t *dst, size_t dst_size,
                                 const uint8_t *src, size_t src_size,
                                 void *scratch, compression_algorithm algorithm);

#endif
