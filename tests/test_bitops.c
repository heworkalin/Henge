/*
 * The same instruction bytes, run twice: once by the CPU and once through the JIT.
 *
 * A run of the firmware's decompressor produces about a megabyte of wrong bytes and then recovers
 * at the next block boundary, while the code it produces is byte for byte the image. That shape
 * says one instruction in the decoder's inner loop computes something else under the JIT than on
 * the CPU.
 *
 * Two programs:
 *   bitops  — extraction, shifts, rotates, byte shuffles, narrowing and widening, compares.
 *   memops  — the match copier: sixteen-byte loads and stores at unaligned addresses, and across
 *             the boundary between two pages, which is the case that takes the engine's slow path.
 *
 * Build and run with android/test-bitops.sh — no device involved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <stdint.h>

#include "cpu.h"
#include "dispatch.h"
#include "machine.h"

#include "bitops_prog_bytes.h"   /* prog_blob[] and the two programs' offsets, generated */

/* Two pages of results: the bit operations use the first 768 bytes, the copier both pages. */
#define OUT_BYTES 8192
/* Where the landing pad (b .) sits inside the code page. */
#define LANDING 3072

static void *g_code;
static uint8_t out_stack[32768] __attribute__((aligned(4096)));
/* Guest RAM for the result, registered once: the JIT side reads and writes this, the native side
   gets an identical copy of the same starting bytes. */
static uint8_t out_buf[8192] __attribute__((aligned(4096)));

static void on_fault(int sig, siginfo_t *si, void *uc) {
    ucontext_t *c = uc;
    printf("fault %d at address %p, pc %#llx x0 %#llx x1 %#llx x30 %#llx\n", sig, si->si_addr,
           (unsigned long long) c->uc_mcontext.pc, (unsigned long long) c->uc_mcontext.regs[0],
           (unsigned long long) c->uc_mcontext.regs[1], (unsigned long long) c->uc_mcontext.regs[30]);
    _exit(2);
}

/* Writable first, executable after: a mapping that is both at once is refused or unusable on
   hardened kernels, and nothing here needs both at the same time. */
static void *exec_page(const void *code, size_t len) {
    void *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    memset(p, 0, 8192);
    memcpy(p, code, len);
    if (mprotect(p, 8192, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect");
        exit(1);
    }
    __builtin___clear_cache((char *) p, (char *) p + len);
    return p;
}

/*
 * Runs one program both ways and compares what it left in the buffer.
 *
 * The buffer starts with the same bytes for both runs, because a program that only partly fills it
 * would otherwise report the untouched tail as a difference.
 */
static int run_one(const char *name, void *code_run, size_t off, size_t len, HvMachine *m,
                   int bytes) {
    static uint8_t want[8192] __attribute__((aligned(4096)));
    for (int i = 0; i < OUT_BYTES; i++) {
        want[i] = (uint8_t) (i * 7 + 3);
        out_buf[i] = want[i];
    }
    void (*native)(void *) = (void (*)(void *)) ((uint8_t *) code_run + off);
    printf("%s: native run...\n", name);
    native(want);
    printf("%s: native ok, jit run...\n", name);

    static HvCpu cpu;
    hv_cpu_reset(&cpu, m);
    cpu.cpacr_el1 = 3u << 20;   /* FP/SIMD enabled at EL0 */
    cpu.sp = (uint64_t)(uintptr_t) out_stack + sizeof out_stack - 256;
    cpu.x[0] = (uint64_t)(uintptr_t) out_buf;
    cpu.x[30] = (uint64_t)(uintptr_t) code_run + LANDING;
    cpu.pc = (uint64_t)(uintptr_t) code_run + off;
    m->stop_pc = (uint64_t)(uintptr_t) code_run + LANDING;

    int r = hv_run(&cpu);
    printf("%s: dispatcher returned %d (pc %#llx elr %#llx far %#llx x9 %#llx x10 %#llx x11 %#llx)\n",
           name, r, (unsigned long long) cpu.pc, (unsigned long long) cpu.elr_el1,
           (unsigned long long) cpu.far_el1, (unsigned long long) cpu.x[9],
           (unsigned long long) cpu.x[10], (unsigned long long) cpu.x[11]);
    if (r != HV_EXIT_HALT) {
        hv_pc_hist_dump(&cpu);   /* which block was running when it went wrong */
        {   /* The last exception transitions, with what the guest was doing. */
            HvStats *st = &m->stats;
            printf("  elchg: nelchg %u exceptions %llu\n", st->nelchg,
                   (unsigned long long) m->stats.exceptions);
            for (int k = 15; k >= 0; k--) {
                uint32_t i = (st->nelchg - 1 - (uint32_t) k) & 15;
                printf("  elchg: from %#llx spsr %#llx kind %u el %u esr %#llx\n",
                       (unsigned long long) st->elchg[i].from, (unsigned long long) st->elchg[i].spsr,
                       st->elchg[i].kind, st->elchg[i].el, (unsigned long long) st->elchg[i].to);
            }
        }
        printf("%s: dispatcher returned %d at pc %#llx (insn %08x) esr %#llx far %#llx\n", name, r,
               (unsigned long long) cpu.pc, cpu.exit_insn, (unsigned long long) cpu.esr_el1,
               (unsigned long long) cpu.far_el1);
        return 1;
    }

    int diffs = 0;
    for (int i = 0; i < bytes; i += 8) {
        uint64_t a, b;
        memcpy(&a, want + i, 8);
        memcpy(&b, out_buf + i, 8);
        if (a != b) {
            if (diffs < 16)
                printf("  %s +%4d native %016llx jit %016llx\n", name, i, (unsigned long long) a,
                       (unsigned long long) b);
            diffs++;
        }
    }
    printf("%-7s %s (%zu bytes of program, %d bytes compared)\n", name, diffs ? "FAIL" : "ok", len,
           bytes);
    return diffs != 0;
}

/* One iteration: fill the operands the same way for both runs, run both, compare results. */
static int fuzz_round(HvMachine *m, void *code_run, int round) {
    static uint8_t want[8192] __attribute__((aligned(4096)));
    uint64_t s = 0x9e3779b97f4a7c15ull * (uint64_t) (round + 1);
    for (int i = 0; i < 0x400; i += 8) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        *(uint64_t *) (want + i) = s;
    }
    memset(want + 0x400, 0xcc, 768);
    memcpy(out_buf, want, sizeof want);

    void (*native)(void *) = (void (*)(void *)) ((uint8_t *) code_run + BITOPS_OFF);
    native(want);

    static HvCpu cpu;
    hv_cpu_reset(&cpu, m);
    cpu.cpacr_el1 = 3u << 20;
    cpu.sp = (uint64_t)(uintptr_t) out_stack + sizeof out_stack - 256;
    cpu.x[0] = (uint64_t)(uintptr_t) out_buf;
    cpu.x[30] = (uint64_t)(uintptr_t) code_run + LANDING;
    cpu.pc = (uint64_t)(uintptr_t) code_run + BITOPS_OFF;
    m->stop_pc = (uint64_t)(uintptr_t) code_run + LANDING;
    int r = hv_run(&cpu);
    if (r != HV_EXIT_HALT) {
        printf("fuzz %d: dispatcher returned %d at pc %#llx\n", round, r, (unsigned long long) cpu.pc);
        return 1;
    }
    for (int i = 0; i < 768; i++) {
        if (want[0x400 + i] != out_buf[0x400 + i]) {
            uint64_t a, b;
            memcpy(&a, want + 0x400 + (i & ~7), 8);
            memcpy(&b, out_buf + 0x400 + (i & ~7), 8);
            printf("fuzz %d: result +%d native %016llx jit %016llx\n", round, i & ~7,
                   (unsigned long long) a, (unsigned long long) b);
            return 1;
        }
    }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    /* The page holds the programs and, past them, a `b .` the dispatcher can stop on. */
    static uint8_t code_copy[8192] __attribute__((aligned(4096)));
    memcpy(code_copy, prog_blob, prog_blob_len);
    ((uint32_t *) code_copy)[LANDING / 4] = 0x14000000u;
    void *code_run = exec_page(code_copy, sizeof code_copy);
    g_code = code_run;

    static HvMachine m;
    if (hv_machine_init(&m, 8 << 20) != 0) {
        printf("machine init failed\n");
        return 1;
    }
    m.log = getenv("HV_LOG") ? atoi(getenv("HV_LOG")) : 0;
    printf("machine: slots %p max_slots %u max_blocks %u jit words %zu\n", (void *) m.slots,
           m.max_slots, m.max_blocks, (size_t) m.jit.capacity);

    hv_machine_add_ram(&m, (uint64_t)(uintptr_t) code_run, 8192, code_run, 1);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t) out_buf, OUT_BYTES, out_buf, 0);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t) out_stack, sizeof out_stack, out_stack, 0);

    int failed = 0;
    /* Operands from the buffer: a thousand rounds is a thousand sets of values through the same
       instruction sequence, and a round costs a few milliseconds. */
    int rounds = getenv("ROUNDS") ? atoi(getenv("ROUNDS")) : 200;
    int bad = 0;
    for (int i = 0; i < rounds && bad < 5; i++) bad += fuzz_round(&m, code_run, i);
    printf("fuzz: %d rounds, %d disagreements\n", rounds, bad);

    const char *only = getenv("ONLY");
    if (!only || !strcmp(only, "bitops"))
        failed += run_one("bitops", code_run, BITOPS_OFF, BITOPS_LEN, &m, 768);
    if (!only || !strcmp(only, "memops"))
        failed += run_one("memops", code_run, MEMOPS_OFF, MEMOPS_LEN, &m, OUT_BYTES);
    if (!only || !strcmp(only, "mathops"))
        failed += run_one("mathops", code_run, MATHOPS_OFF, MATHOPS_LEN, &m, 25 * 8);
    if (!only || !strcmp(only, "atomics"))
        failed += run_one("atomics", code_run, ATOMICS_OFF, ATOMICS_LEN, &m, 32 * 8);
    printf("%d of 4 programs differ\n", failed);
    return failed != 0;
}
