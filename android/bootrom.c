/*
 * Unpacks AVPBooter.vmapple2.bin out of a range of Apple's 14.6.1 restore image.
 *
 * The path is the one the iOS app takes, and the numbers in bootrom.h come from reading that
 * image's disk layout: a 1 MiB LZFSE block of the system DMG holds an APFS compressed file's
 * resource fork, and the fork is a table of offsets followed by 64 KiB pieces that are either
 * stored as they are or LZVN-compressed. Both decompressors are lzfse's, which is why this port
 * vendors that library instead of reimplementing two formats.
 *
 * The SHA-256 check is not paranoia: the whole extraction depends on byte offsets into a file
 * Apple controls, and if Apple ever republishes the image differently those offsets point at
 * something else. A hash turns that into a clear failure rather than a Mac that will not boot.
 */
#include "bootrom.h"

#include <string.h>

#include "lzfse.h"
#include "mbedtls/sha256.h"

static const char kExpectedSha256[] =
        "fd4973e42e609751830df6f3dd2d2b2ad2309fe38f0ca0f5aba02dcbefe45f91";

const char *hv_bootrom_sha256(void) {
    return kExpectedSha256;
}

/*
 * LZVN, which is the format inside the pieces.
 *
 * The lzfse library decodes LZVN as part of an LZFSE stream but does not expose a whole-buffer
 * entry point for it, and pulling in its streaming state machine to decode 64 KiB pieces would be
 * more code than the format is. This is the decoder the iOS app already uses, which was checked
 * against the real ROM's hash; it is a port, not a guess.
 *
 * Every branch that cannot be satisfied returns -1 rather than reading past the input: this runs on
 * data from the network.
 */
static int lzvn_decode(const uint8_t *s, size_t n, uint8_t *out, size_t expected) {
    size_t i = 0, o = 0;
    size_t distance = 0;
    while (i < n) {
        int op = s[i];
        size_t literal = 0, match = 0, length = 1;
        if (op == 0x06) return o == expected ? 0 : -1;
        if (op == 0x0e || op == 0x16) {
            i++;
            continue;
        }
        if (op == 0x1e || op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e
            || (op >= 0x70 && op <= 0x7f) || (op >= 0xd0 && op <= 0xdf)) {
            return -1;
        }
        if (op == 0xe0) {
            if (i + 1 >= n) return -1;
            literal = (size_t) s[i + 1] + 16;
            length = 2;
        } else if (op >= 0xe1 && op <= 0xef) {
            literal = (size_t) (op & 15);
        } else if (op == 0xf0) {
            if (i + 1 >= n) return -1;
            match = (size_t) s[i + 1] + 16;
            length = 2;
        } else if (op >= 0xf1) {
            match = (size_t) (op & 15);
        } else if (op >= 0xa0 && op <= 0xbf) {
            if (i + 2 >= n) return -1;
            unsigned word = (unsigned) s[i + 1] | ((unsigned) s[i + 2] << 8);
            literal = (size_t) (op >> 3) & 3;
            match = (size_t) (((op & 7) << 2) | (int) (word & 3)) + 3;
            distance = word >> 2;
            length = 3;
        } else {
            literal = (size_t) (op >> 6) & 3;
            match = (size_t) ((op >> 3) & 7) + 3;
            switch (op & 7) {
            case 6:
                break;                       /* the distance used last time */
            case 7:
                if (i + 2 >= n) return -1;
                distance = (size_t) s[i + 1] | ((size_t) s[i + 2] << 8);
                length = 3;
                break;
            default:
                if (i + 1 >= n) return -1;
                distance = ((size_t) (op & 7) << 8) | (size_t) s[i + 1];
                length = 2;
                break;
            }
        }
        i += length;
        if (i + literal > n || o + literal + match > expected) return -1;
        memcpy(out + o, s + i, literal);
        i += literal;
        o += literal;
        if (match > 0) {
            if (distance == 0 || distance > o) return -1;
            /* Byte at a time on purpose: a match may overlap what it is writing. */
            for (size_t k = 0; k < match; k++, o++) out[o] = out[o - distance];
        }
    }
    return o == expected ? 0 : -1;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16)
           | ((uint32_t) p[3] << 24);
}

int hv_bootrom_unpack(const uint8_t *block, size_t block_len,
                      uint8_t *rom, size_t rom_cap) {
    if (!block || !rom || rom_cap < HV_BOOTROM_BYTES) return -1;
    if (block_len < HV_BOOTROM_BLOCK_BYTES) return -1;

    /* 1 MiB of scratch on the heap: this runs once per install, and a megabyte on a worker
     * thread's stack is not worth the risk. */
    static uint8_t whole[HV_BOOTROM_UNPACKED];
    size_t unpacked = lzfse_decode_buffer(whole, sizeof(whole), block,
                                          HV_BOOTROM_BLOCK_BYTES, NULL);
    if (unpacked != sizeof(whole)) return -1;

    /* 2. The fork: a table of offsets, the last entry ending the pieces. */
    const uint8_t *fork = whole + HV_BOOTROM_FORK_OFFSET;
    size_t fork_len = HV_BOOTROM_FORK_BYTES;
    uint32_t table_len = le32(fork);
    if (table_len < 8 || (table_len % 4) != 0 || table_len > fork_len) return -2;

    size_t out = 0;
    for (uint32_t at = 0; at + 4 < table_len; at += 4) {
        uint32_t start = le32(fork + at);
        uint32_t end = le32(fork + at + 4);
        if (start >= end || end > fork_len) return -2;

        size_t want = 65536;
        if (want > HV_BOOTROM_BYTES - out) want = HV_BOOTROM_BYTES - out;
        if (want == 0) return -3;

        const uint8_t *piece = fork + start;
        size_t piece_len = end - start;
        if (piece[0] == 0x06) {
            /* Stored: the bytes follow the type byte as they are. */
            if (piece_len - 1 < want) return -2;
            memcpy(rom + out, piece + 1, want);
            out += want;
        } else {
            if (lzvn_decode(piece, piece_len, rom + out, want) != 0) return -2;
            out += want;
        }
    }
    if (out != HV_BOOTROM_BYTES) return -3;

    uint8_t digest[32];
    mbedtls_sha256((const unsigned char *) rom, HV_BOOTROM_BYTES, digest, 0);
    static const char hex[] = "0123456789abcdef";
    char text[65];
    for (int i = 0; i < 32; i++) {
        text[i * 2] = hex[digest[i] >> 4];
        text[i * 2 + 1] = hex[digest[i] & 15];
    }
    text[64] = '\0';
    if (strcmp(text, kExpectedSha256) != 0) return -4;
    return 0;
}
