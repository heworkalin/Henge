#ifndef HENGE_ANDROID_BOOTROM_H
#define HENGE_ANDROID_BOOTROM_H

#include <stddef.h>
#include <stdint.h>

/*
 * The virtual Mac's boot ROM, unpacked from one compressed block of an Apple restore image.
 *
 * The ROM is not a file inside an IPSW. It is one 1 MiB LZFSE block of the encrypted system image,
 * and inside that block a compressed-file fork whose pieces are LZVN. Apple ships it; this port
 * keeps the same position the iOS app does — it is fetched from Apple rather than redistributed —
 * and the same hash check, so a byte wrong anywhere fails loudly instead of booting something
 * unexpected.
 */
#define HV_BOOTROM_BLOCK_BYTES 859848u    /* the compressed block, as stored in the image */
#define HV_BOOTROM_UNPACKED 1048576u      /* that block, decompressed */
#define HV_BOOTROM_FORK_OFFSET 876544u    /* where the compressed file's fork sits inside it */
#define HV_BOOTROM_FORK_BYTES 116335u
#define HV_BOOTROM_BYTES 214904u          /* the ROM itself */

/*
 * Unpacks the block `block` (HV_BOOTROM_BLOCK_BYTES of it) into `rom` (HV_BOOTROM_BYTES).
 *
 * Returns 0 on success. Returns -1 when the block does not decompress, -2 when the fork is
 * malformed, -3 when the result is not HV_BOOTROM_BYTES, -4 when its SHA-256 is not the expected
 * one. The caller writes the file only on 0.
 */
int hv_bootrom_unpack(const uint8_t *block, size_t block_len,
                      uint8_t *rom, size_t rom_cap);

/* The SHA-256 the ROM must have, as hex. */
const char *hv_bootrom_sha256(void);

#endif
