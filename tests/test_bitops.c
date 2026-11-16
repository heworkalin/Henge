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
#include <pthread.h>
#include <unistd.h>

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
/* `reloc`: the program stores pointers into the buffer. Native and JIT run over two
   different buffers, so those words differ by the buffer base alone; subtract each run's
   own base before comparing, or the test reports the buffer address as a difference. */
static uint64_t rebase(uint64_t v, const void *base) {
    uint64_t b = (uint64_t) (uintptr_t) base;
    return (v >= b && v < b + OUT_BYTES) ? v - b : v;
}

static int run_one(const char *name, void *code_run, size_t off, size_t len, HvMachine *m,
                   int bytes, int reloc) {
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
        if (reloc) {
            a = rebase(a, want);
            b = rebase(b, out_buf);
        }
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

/*
 * Multi-core scheduler handoff: two vCPUs, two host threads, one shared guest buffer.
 *
 * The programs under test are in tests/bitops_prog.s (sched_mc_waiter / sched_mc_waker);
 * this is only the scaffolding that starts two CPUs at once, because a pthread cannot be
 * created from assembly.
 *
 * It reproduces the guest's stall shape: the waiter publishes TH_WAIT and spins for TH_RUN,
 * the waker clears TH_WAIT and sets TH_RUN. If the wake does not reach the other core, the
 * waiter never leaves its spin -- the test times out and says so, instead of hanging.
 */
struct mc_ctx {
    HvCpu *cpu;
    int ret;
};

static void *mc_thread(void *arg) {
    struct mc_ctx *c = arg;
    c->ret = hv_run(c->cpu);
    return NULL;
}

static int run_mc(HvMachine *m, void *code_run) {
    static HvCpu cpus[2];
    static struct mc_ctx ctx[2];
    pthread_t th[2];

    memset(out_buf, 0, 0x40);

    for (int i = 0; i < 2; i++) {
        hv_cpu_reset(&cpus[i], m);
        cpus[i].index = (unsigned) i;
        cpus[i].cpacr_el1 = 3u << 20;
        /* One stack per CPU, far enough apart that a deep spin cannot touch the other. */
        cpus[i].sp = (uint64_t)(uintptr_t) out_stack + (i ? 16384 : 32768) - 256;
        cpus[i].x[0] = (uint64_t)(uintptr_t) out_buf;
        cpus[i].x[30] = (uint64_t)(uintptr_t) code_run + LANDING;
        cpus[i].pc = (uint64_t)(uintptr_t) code_run + (i == 0 ? MCW_OFF : MCT_OFF);
        ctx[i].cpu = &cpus[i];
        ctx[i].ret = -99;
    }
    m->stop_pc = (uint64_t)(uintptr_t) code_run + LANDING;

    if (pthread_create(&th[0], NULL, mc_thread, &ctx[0]) != 0 ||
        pthread_create(&th[1], NULL, mc_thread, &ctx[1]) != 0) {
        printf("mc: cannot start a thread\n");
        return 1;
    }

    /* Wait for both sides, with a bound: a waiter that never observes TH_RUN is the bug. */
    int ticks = 0, both = 0;
    for (; ticks < 300; ticks++) {
        uint32_t w, t;
        memcpy(&w, out_buf + 8, 4);
        memcpy(&t, out_buf + 12, 4);
        if (w && t) {
            both = 1;
            break;
        }
        usleep(10000);   /* 10 ms, 300 times: 3 s */
    }

    uint32_t state, ready, wd, td;
    memcpy(&state, out_buf + 0, 4);
    memcpy(&ready, out_buf + 4, 4);
    memcpy(&wd, out_buf + 8, 4);
    memcpy(&td, out_buf + 12, 4);

    if (!both) {
        /* The spinning CPU is still inside hv_run and cannot be joined; leave it and report.
           The process exits below, so no thread is left holding the machine. */
        printf("mc: TIMEOUT after %.1f s: state 0x%x ready %u waker %u waiter %u\n",
               ticks * 0.01, state, ready, wd, td);
        return 1;
    }
    pthread_join(th[0], NULL);
    pthread_join(th[1], NULL);
    int ok = (state & 0x4) && !(state & 0x1) && td == 1 && wd == 1;
    printf("mc: state 0x%x (TH_RUN %s, TH_WAIT %s) ready %u waker %u waiter %u -> %s\n",
           state, (state & 0x4) ? "set" : "CLEAR", (state & 0x1) ? "still set" : "cleared",
           ready, wd, td, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
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
        failed += run_one("bitops", code_run, BITOPS_OFF, BITOPS_LEN, &m, 768, 0);
    if (!only || !strcmp(only, "memops"))
        failed += run_one("memops", code_run, MEMOPS_OFF, MEMOPS_LEN, &m, OUT_BYTES, 0);
    if (!only || !strcmp(only, "mathops"))
        failed += run_one("mathops", code_run, MATHOPS_OFF, MATHOPS_LEN, &m, 25 * 8, 0);
    if (!only || !strcmp(only, "atomics"))
        failed += run_one("atomics", code_run, ATOMICS_OFF, ATOMICS_LEN, &m, 32 * 8, 0);
        /* The scheduler's list operations: enqueue, dequeue, clear_wait. */
        failed += run_one("sched", code_run, SCHED_OFF, SCHED_LEN, &m, 0x90, 1);
    if (!only || !strcmp(only, "mc"))
        failed += run_mc(&m, code_run);
    printf("%d of 4 programs differ\n", failed);
    return failed != 0;
}
