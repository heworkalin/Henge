/*
 * Decodes the real kernel payload twice — natively, and as guest code through the JIT — and
 * compares the two results byte for byte.
 *
 * This is the experiment the device cannot run quickly: the firmware's own decompressor is the
 * thing suspected of producing a megabyte of wrong bytes, and the difference between "our
 * emulation of the decoder is wrong" and "the decoder is right and something else corrupted the
 * image" is one comparison.
 *
 * The decoder here is Apple's open-source LZFSE (the same code path the firmware uses for a 'bvx2'
 * stream). It runs as guest code with the JIT because this harness maps guest memory at the host
 * addresses it already has: everything the decoder touches — its text, its tables, the buffers —
 * is in the test binary and is registered as guest RAM, so a guest address is a host address and
 * the payload does not have to move.
 *
 * Build and run with android/test-lzfse.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cpu.h"
#include "dispatch.h"
#include "machine.h"
#include "exception.h"

/* The decoder and the buffers live in the binary; nothing here calls the C library, so what runs
   as guest code never leaves the registered range. */
extern size_t lzfse_decode_buffer(uint8_t *dst, size_t dst_size, const uint8_t *src, size_t src_size,
                                  void *scratch);
extern size_t lzfse_decode_scratch_size(void);

/* The ranges of this executable, from the linker. */
extern char __executable_start[];
extern char _end[];

#define DST_BYTES (80u << 20)
static uint8_t dst_native[DST_BYTES] __attribute__((aligned(4096)));
static uint8_t dst_jit[DST_BYTES] __attribute__((aligned(4096)));
static uint8_t src_copy[24u << 20] __attribute__((aligned(4096)));
static uint8_t scratch[1u << 20] __attribute__((aligned(4096)));
static uint8_t landing_page[4096] __attribute__((aligned(4096)));
static uint8_t gstack[65536] __attribute__((aligned(4096)));

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        printf("usage: %s payload.lz [reference.bin]\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    size_t src_size = fread(src_copy, 1, sizeof src_copy, f);
    fclose(f);
    printf("payload %zu bytes, scratch %zu\n", src_size, lzfse_decode_scratch_size());
    printf("dst_jit %p src_copy %p scratch %p scratch_size %zu\n", (void *) dst_jit,
           (void *) src_copy, (void *) scratch, lzfse_decode_scratch_size());

    /* Native run: the reference. */
    memset(dst_native, 0xcc, sizeof dst_native);
    size_t n_native = lzfse_decode_buffer(dst_native, sizeof dst_native, src_copy, src_size, scratch);
    printf("native decoded %zu bytes\n", n_native);

    /* The same call as guest code. */
    static HvMachine m;
    if (hv_machine_init(&m, 256u << 20) != 0) {
        printf("machine init failed\n");
        return 1;
    }
    uint64_t lo = (uint64_t)(uintptr_t) __executable_start & ~0xfffull;
    uint64_t hi = ((uint64_t)(uintptr_t) _end + 0xfff) & ~0xfffull;
    if (hv_machine_add_ram(&m, lo, hi - lo, (void *)(uintptr_t) lo, 0) != 0) {
        printf("registering the binary as guest RAM failed (%#llx..%#llx)\n",
               (unsigned long long) lo, (unsigned long long) hi);
        return 1;
    }
    printf("machine %p (%zu bytes) gstack %p..%p\n", (void *) &m, sizeof m, (void *) gstack,
           (void *) (gstack + sizeof gstack));
    printf("guest RAM: the executable, %#llx..%#llx (%llu MB)\n", (unsigned long long) lo,
           (unsigned long long) hi, (unsigned long long)((hi - lo) >> 20));

    ((uint32_t *) landing_page)[0] = 0x14000000u;   /* b . */
    uint64_t landing = (uint64_t)(uintptr_t) landing_page;

    memset(dst_jit, 0xcc, sizeof dst_jit);
    static HvCpu cpu;
    hv_cpu_reset(&cpu, &m);
    cpu.cpacr_el1 = 3u << 20;
    cpu.sp = (uint64_t)(uintptr_t) gstack + sizeof gstack - 256;
    cpu.x[0] = (uint64_t)(uintptr_t) dst_jit;
    cpu.x[1] = sizeof dst_jit;
    cpu.x[2] = (uint64_t)(uintptr_t) src_copy;
    cpu.x[3] = src_size;
    cpu.x[4] = (uint64_t)(uintptr_t) scratch;
    cpu.x[30] = landing;
    cpu.pc = (uint64_t)(uintptr_t) lzfse_decode_buffer;
    m.stop_pc = landing;
    m.log = getenv("HV_LOG") ? atoi(getenv("HV_LOG")) : 0;

    int r = hv_run(&cpu);
    printf("jit returned %d (x0 = %llu, pc %#llx elr %#llx esr %#llx far %#llx x30 %#llx)\n", r,
           (unsigned long long) cpu.x[0], (unsigned long long) cpu.pc,
           (unsigned long long) cpu.elr_el1, (unsigned long long) cpu.esr_el1,
           (unsigned long long) cpu.far_el1, (unsigned long long) cpu.x[30]);
    if (r != HV_EXIT_HALT) {
        for (int i = 0; i < 31; i += 4)
            printf("  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx\n", i,
                   (unsigned long long) cpu.x[i], i + 1, (unsigned long long) cpu.x[i + 1], i + 2,
                   (unsigned long long) cpu.x[i + 2], i + 3, (unsigned long long) cpu.x[i + 3]);
        printf("  sp %#llx  jit base %p  jit capacity %zu words\n", (unsigned long long) cpu.sp,
               (void *) m.jit.base, (size_t) m.jit.capacity);
        hv_pc_hist_dump(&cpu);
    }

    size_t n_jit = (size_t) cpu.x[0];
    int bad = 0;
    if (n_jit != n_native) {
        printf("length differs: native %zu, jit %zu\n", n_native, n_jit);
        bad++;
    }
    size_t cmp = n_native < n_jit ? n_native : n_jit;
    size_t first = (size_t)-1, count = 0;
    for (size_t i = 0; i < cmp; i++)
        if (dst_native[i] != dst_jit[i]) {
            if (first == (size_t)-1) first = i;
            count++;
        }
    if (count) {
        printf("output differs: %zu of %zu bytes, first at %#zx\n", count, cmp, first);
        bad++;
    }
    if (argc > 2) {
        /* And against the image the file itself expands to, which is what the guest should have. */
        FILE *rf = fopen(argv[2], "rb");
        if (rf) {
            static uint8_t want[DST_BYTES];
            size_t n_want = fread(want, 1, sizeof want, rf);
            fclose(rf);
            size_t c = n_want < n_native ? n_want : n_native;
            size_t d = 0, f0 = (size_t)-1;
            for (size_t i = 0; i < c; i++)
                if (want[i] != dst_native[i]) {
                    if (f0 == (size_t)-1) f0 = i;
                    d++;
                }
            printf("native vs reference: %zu of %zu differ%s%#zx\n", d, c,
                   d ? ", first at " : "", d ? f0 : 0);
        }
    }
    printf("%s\n", bad ? "LZFSE FAIL" : "LZFSE ok");
    return bad != 0;
}
