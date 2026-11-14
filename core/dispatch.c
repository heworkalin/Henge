#include "dispatch.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>

#include "exception.h"
#include "memop.h"
#include "mmu.h"
#include "sysreg.h"
#include "translate.h"

void hv_flush_blocks(HvMachine *m) {
    /* Every other CPU is parked (stop_the_world): the one moment the TLB layout can change. */
    if ((hv_knob[HV_K_TLBT] != 0) != hv_tlb_tmajor) {
        hv_tlb_tmajor = hv_knob[HV_K_TLBT] != 0;
        for (int i = 0; i < m->ncpus; i++)
            if (m->cpus[i]) {
                m->cpus[i]->nkernel_fills = HV_KERNEL_FILLS + 1; /* the logs hold slots of the old layout: clear it all */
                hv_tlb_flush(m->cpus[i]);
            }
    }
    m->stats.block_flushes++;
    __atomic_add_fetch(&m->jit_gen, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < m->ncpus; i++)
        if (m->cpus[i]) {
            hv_ijc_reset(m->cpus[i], 0);
            hv_ijc_reset(m->cpus[i], 1);
            hv_ret_clear(m->cpus[i]); /* they point at code that is going */
            memset(m->percpu[i].jc, 0, sizeof m->percpu[i].jc);
        }
    memset(m->hash, 0, sizeof m->hash);
    m->nblocks = 0;
    m->nslots = 0;
    m->nxchained = 0;
    for (int i = 0; i < m->nram; i++) memset(m->ram[i].xhead, 0, m->ram[i].npages * sizeof *m->ram[i].xhead); /* slots restart from 0 */
    hv_jit_reset(&m->jit);
    m->jit.limit = hv_knob[HV_K_JITCAP] > 0 ? ((size_t)hv_knob[HV_K_JITCAP] << 20) / sizeof(uint32_t) : 0; /* MB of it to use (testing) */
    /* HV_JIT_POISON: nothing old is left to run, so a stale pointer faults where it is used. */
    static int poison = -1;
    if (poison < 0) poison = getenv("HV_JIT_POISON") != NULL;
    m->gen_checks = poison;
    if (poison) {
        size_t bytes = (size_t)m->jit.capacity * 4;
        uint32_t *rw = (uint32_t *)((uint8_t *)m->jit.base + m->jit.rw_diff);
        for (size_t i = 0; i < bytes / 4; i++) rw[i] = 0;
        hv_jit_finish(&m->jit, m->jit.base, m->jit.capacity);
    }
}

/* Parked: not running guest code, nor using its TLB, so other CPUs need not
   wait for it (TLBIs, a flush). It catches up with requests on the way back. */
/* A kernel collection arrives with chained fixups that the *loader* is supposed to have resolved:
   iBoot rebases every slot and signs the authenticated ones, and the kernel then skips its own pass
   because the sub-image says it is already in the cache (the guard reads the Mach-O flags of the
   fileset entry and branches over the fixup loop). Only the rebasing has to happen here — guest
   pointers are never signed in this engine, and every AUT* the guest runs strips the signature
   field anyway.
   Pointer format 8, DYLD_CHAINED_PTR_64_KERNEL_CACHE: target:30, cacheLevel:2, diversity:16,
   addrDiv:1, key:2, next:12 (in 4-byte units), isAuth:1. `target` is an offset from the image
   base, and the chain start offsets are per page in a table after the segment header. */
static uint32_t kf_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t kf_u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint16_t kf_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static int kf_is_kernel(const uint8_t *h) {
    if (kf_u32(h) != 0xfeedfacf) return 0;
    uint32_t ncmds = kf_u32(h + 16), off = 32;
    for (uint32_t c = 0; c < ncmds && off + 72 <= 4096; c++) {
        uint32_t cmd = kf_u32(h + off), csz = kf_u32(h + off + 4);
        if (csz < 8 || off + csz > 4096) return 0;
        if (cmd == 0x19 && !memcmp(h + off + 8, "__TEXT", 7) && kf_u64(h + off + 24) == 0xfffffe0007004000ull) return 1;
        off += csz;
    }
    return 0;
}

/* 64 bytes of real entropy for the guest's boot seed. The host has it (>= getrandom(2) or
   /dev/urandom); nothing in the guest can produce it. */
static int derive_seed(uint8_t *out, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    if (fd >= 0) {
        while (got < n) {
            ssize_t r = read(fd, out + got, n - got);
            if (r <= 0) break;
            got += (size_t) r;
        }
        close(fd);
    }
    if (got == n) return 1;
    uint64_t x = mach_absolute_time() ^ ((uint64_t) getpid() << 32) ^ (uint64_t) (uintptr_t) out;
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        out[i] = (uint8_t) x;
    }
    return 1;
}

static uint64_t kf_dc_pa;   /* where __DATA_CONST starts, for the beat probe below */
static uint64_t dc_last, dc_poll;
static int hv_dc_probe = -1;   /* HV_DC_PROBE=1: report every change of the first __DATA_CONST word */

static void kernel_fixups(HvMachine *m, uint64_t image_pa, uint64_t image_va) {
    kf_dc_pa = image_pa + 0x6f8000;
    uint8_t hdr[4096];
    if (hv_phys_read(m, image_pa, hdr, sizeof hdr) != 0) return;
    uint32_t ncmds = kf_u32(hdr + 16), off = 32, csz_fix = 0, fix_off = 0, nseg = 0;
    uint64_t lo = 0, hi = 0, text_va = 0;
    char segname[8][17];
    memset(segname, 0, sizeof segname);
    for (uint32_t c = 0; c < ncmds && off + 8 <= sizeof hdr; c++) {
        uint32_t cmd = kf_u32(hdr + off), csz = kf_u32(hdr + off + 4);
        if (csz < 8 || off + csz > sizeof hdr) break;
        if (cmd == 0x80000034) { fix_off = kf_u32(hdr + off + 8); csz_fix = kf_u32(hdr + off + 12); }
        if (cmd == 0x19 && nseg < 8) { /* LC_SEGMENT_64: the starts table is indexed in this order */
            memcpy(segname[nseg], hdr + off + 8, 16);
            segname[nseg][16] = 0;
            /* The image's own span in the file's own terms, for the sanity check below. */
            uint64_t sva = kf_u64(hdr + off + 24), svs = kf_u64(hdr + off + 32);
            if (!nseg) text_va = sva;
            if (!lo || sva < lo) lo = sva;
            if (sva + svs > hi) hi = sva + svs;
            nseg++;
        }
        off += csz;
    }
    if (!csz_fix || csz_fix > (8u << 20)) return;
    uint8_t *fx = malloc(csz_fix);
    if (!fx) return;
    if (hv_phys_read(m, image_pa + fix_off, fx, csz_fix) != 0) { free(fx); return; }
    uint32_t starts = kf_u32(fx + 4);
    uint32_t segs_in_fix = kf_u32(fx + starts);
    uint32_t applied = 0, already = 0, outside = 0, slots = 0;
    uint32_t seg_applied[8] = {0}, seg_already[8] = {0};
    for (uint32_t s = 0; s < segs_in_fix; s++) {
        /* Both __DATA and __DATA_CONST have fixup chains and neither is rebased by the time the
           kernel starts: a raw chain word left in __DATA_CONST was dereferenced as a string
           pointer and the guest died in a data abort before it printed anything. The slot is only
           rewritten when it does not already hold a pointer, so a pass that did run is not
           applied twice. HV_NO_KFIX_DC=1 leaves __DATA_CONST alone for comparison. */
        /* __DATA_CONST is left to the kernel: at icount ~57k the guest itself walks those chains
           and adds the slide to whatever it finds there (pc 0x1d177750), which turns a slot this
           pass had already rebased into base+pointer and the guest branches into nothing. iBoot
           hands the image over with those slots still raw, which is what that kernel pass expects.
           HV_KFIX_DC=1 forces the pass here, for comparison. */
        if (s < 8 && !strcmp(segname[s], "__DATA_CONST") && !getenv("HV_KFIX_DC")) continue;
        uint32_t soff = kf_u32(fx + starts + 4 + 4 * s);
        if (!soff) continue;
        const uint8_t *sp = fx + starts + soff;
        if (sp + 22 > fx + csz_fix) continue;
        uint16_t psize = kf_u16(sp + 4), pfmt = kf_u16(sp + 6);
        uint64_t seg_off = kf_u64(sp + 8);
        uint16_t pages = kf_u16(sp + 20);
        if (pfmt != 8 || !psize) continue;
        for (uint16_t pg = 0; pg < pages; pg++) {
            if (sp + 22 + 2 * pg + 2 > fx + csz_fix) break;
            uint16_t start = kf_u16(sp + 22 + 2 * pg);
            if (start == 0xffff) continue;
            uint64_t o = start;
            for (;;) {
                uint64_t pa = image_pa + seg_off + (uint64_t) pg * psize + o;
                uint64_t v = 0;
                if (hv_phys_read(m, pa, &v, 8) != 0) break;
                uint64_t target = v & 0x3fffffffull;
                uint32_t next = (uint32_t) ((v >> 51) & 0xfff);
                slots++;
                if (v >= 0xfffffe0000000000ull) { already++; if (s < 8) seg_already[s]++; } /* a pointer already: the loader got here */
                else if (target >= hi - text_va) {
                    /* A target past the end of the image is not a fixup: the walk ran into data
                       that happens to look like a chain. Writing base+target would leave an
                       address no segment covers, and the guest branches to it. Left alone. */
                    outside++;
                }
                else {
                    uint64_t nv = image_va + target;
                    hv_phys_write(m, pa, &nv, 8);
                    applied++;
                    if (s < 8) seg_applied[s]++;
                }
                if (!next) break;
                o += (uint64_t) next * 4;
                if (o >= psize) break;
            }
        }
    }
    free(fx);
    hv_phys_read(m, kf_dc_pa, &dc_last, 8);
    hv_dc_probe = getenv("HV_DC_PROBE") != NULL;
    /* Read back the first slots of __DATA_CONST: if they hold the base twice by the time the
       guest reaches them, something rebased on top of this pass. */
    uint64_t probe[4] = {0, 0, 0, 0};
    hv_phys_read(m, image_pa + 0x6f8000, probe, sizeof probe);
    const char *wd = getenv("HV_BLOCK_DUMP");
    if (wd && *wd) {
        char p2[512];
        snprintf(p2, sizeof p2, "%s", wd);
        char *sl = strrchr(p2, '/');
        if (sl) snprintf(sl + 1, sizeof p2 - (size_t) (sl + 1 - p2), "fixups.txt");
        FILE *f = fopen(p2, "w");
        if (f) {
            fprintf(f, "image pa %llx va %llx\nslots %u applied %u already %u outside %u\n",
                    (unsigned long long) image_pa, (unsigned long long) image_va, slots, applied,
                    already, outside);
            fprintf(f, "dataconst[0..3] %#llx %#llx %#llx %#llx\n",
                    (unsigned long long) probe[0], (unsigned long long) probe[1],
                    (unsigned long long) probe[2], (unsigned long long) probe[3]);
            for (int i = 0; i < 8; i++)
                if (seg_applied[i] || seg_already[i]) fprintf(f, "seg%d: applied %u already %u\n", i, seg_applied[i], seg_already[i]);
            fclose(f);
        }
    }
}

/* A signal raised from inside the JIT region: the translator emitted something the host refuses to
   run (SIGILL), or one of its loads and stores went outside what it should touch. The dispatcher
   only knows the block it entered, not how far into it the CPU got, so the block is found by
   binary search over the block table (which is laid out in JIT order) and what it was made from is
   written out next to the block dump. */
static HvMachine *g_fault_m;

static void jit_fault(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    uint64_t pc = (uint64_t) uc->uc_mcontext.pc;
    HvMachine *m = g_fault_m;
    const char *wd = getenv("HV_BLOCK_DUMP");
    FILE *f = NULL;
    if (wd && *wd) {
        char p[512];
        snprintf(p, sizeof p, "%s", wd);
        char *sl = strrchr(p, '/');
        if (sl) snprintf(sl + 1, sizeof p - (size_t) (sl + 1 - p), "jitcrash.txt");
        f = fopen(p, "a");
    }
    if (f) fprintf(f, "signal %d fault %p pc %#llx\n", sig, si->si_addr, (unsigned long long) pc);
    if (m && pc >= (uint64_t) (uintptr_t) m->jit.base && pc < (uint64_t) (uintptr_t) (m->jit.base + m->jit.used) && m->nblocks) {
        int lo = 0, hi = m->nblocks - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if ((uint64_t) (uintptr_t) m->blocks[mid].code <= pc) lo = mid;
            else hi = mid - 1;
        }
        HvBlock *b = &m->blocks[lo];
        uint64_t off_in_block = (pc - (uint64_t) (uintptr_t) b->code) / 4;
        if (f) {
            fprintf(f, "block va %#llx flags %#x ninsns %u word %llu\n",
                    (unsigned long long) b->va, b->flags, b->ninsns, (unsigned long long) off_in_block);
            const uint32_t *gw = (const uint32_t *) ((const uint8_t *) b->ram->host + (b->pa - b->ram->pa));
            fprintf(f, "guest:");
            for (uint32_t i = 0; i < b->ninsns && i < 64; i++) fprintf(f, " %08x", gw[i]);
            fprintf(f, "\nhost:");
            for (int64_t i = (int64_t) off_in_block - 16; i < (int64_t) off_in_block + 16; i++)
                if (i >= 0) fprintf(f, " %08x", b->code[i]);
            fprintf(f, "\n");
            if (hv_self) {
                fprintf(f, "guest regs: pc %#llx el%u\n", (unsigned long long) hv_self->pc, hv_self->el);
                for (int i = 0; i < 31; i++) fprintf(f, "x%d %#llx%s", i, (unsigned long long) hv_self->x[i], (i % 4 == 3) ? "\n" : "  ");
                fprintf(f, "sp %#llx spsr %#llx\n", (unsigned long long) hv_self->sp, (unsigned long long) hv_self->spsr_el1);
            }
            fclose(f);
        }
    } else if (f) fclose(f);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void jit_fault_watch(HvMachine *m) {
    static int once;
    if (once) return;
    once = 1;
    g_fault_m = m;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = jit_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

static void park(HvCpu *cpu) {
    HvPerCpu *p = cpu->pc_state;
    __atomic_store_n(&p->tlb_live, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&p->active, 0, __ATOMIC_SEQ_CST);
    hv_cpu_ack_wake(cpu); /* a CPU asleep waiting for this one's acknowledgement need not wait any more */
}

static void unpark(HvCpu *cpu) {
    HvPerCpu *p = cpu->pc_state;
    HvMachine *m = cpu->m;
    /* A flush rewrites every CPU's branch caches and their salts, taking "parked"
       to mean nobody is using them. So not back while one runs: hv_cpu_process_requests
       below may reach them, and a salt read before the flush and stored after it
       brings the entries of dropped translations back to life. */
    for (;;) {
        __atomic_store_n(&p->active, 1, __ATOMIC_SEQ_CST);
        if (!__atomic_load_n(&m->stw_active, __ATOMIC_SEQ_CST)) break;
        __atomic_store_n(&p->active, 0, __ATOMIC_SEQ_CST);
        pthread_mutex_lock(&m->stw_mu);
        while (m->stw_active) pthread_cond_wait(&m->stw_cv, &m->stw_mu);
        pthread_mutex_unlock(&m->stw_mu);
    }
    __atomic_store_n(&p->tlb_live, 1, __ATOMIC_SEQ_CST);
    hv_cpu_process_requests(cpu); /* what came in while it was parked */
}

static uint32_t block_flags(HvCpu *cpu) {
    uint32_t flags = cpu->el ? HV_BLOCK_EL1 : 0;
    if (cpu->el && cpu->spsel) flags |= HV_BLOCK_SPX;
    if (cpu->el && (cpu->pstate_extra & HV_PSTATE_PAN)) flags |= HV_BLOCK_PAN;
    uint32_t fpen = (cpu->cpacr_el1 >> 20) & 3;
    if (fpen == 0 || fpen == 2 || (fpen == 1 && cpu->el == 0)) flags |= HV_BLOCK_FPOFF;
    return flags;
}


/* Finding a block is lock-free (blocks are only ever added, with a release
   store, until a flush, which stops every CPU); making one takes the
   translation lock. Returns NULL if the translations must all be dropped first
   (m->flush_pending is set: the dispatcher does that, then tries again). */
#define hash_of hv_block_hash
#define find hv_block_find
/* HV_BLOCK_DUMP=file: every 64th (HV_BLOCK_DUMP_EVERY) translated block (guest and host words) for
   bench/hvdis.py, to see what the translator makes of real code. */
static void block_dump(const HvBlock *b, const uint32_t *code) {
    static FILE *f;
    static int on = -1;
    static uint32_t count, every = 64; /* HV_BLOCK_DUMP_EVERY: the sampling */
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    if (on < 0) {
        const char *path = getenv("HV_BLOCK_DUMP");
        on = path && (f = fopen(path, "wb")) != NULL;
        if (getenv("HV_BLOCK_DUMP_EVERY")) every = (uint32_t)atoi(getenv("HV_BLOCK_DUMP_EVERY"));
        if (!every) every = 1;
    }
    if (!on || __atomic_add_fetch(&count, 1, __ATOMIC_RELAXED) % every) return;
    uint64_t head[3] = {b->va, b->ninsns, 0};
    const uint32_t *w = b->code;
    uint32_t n = hv_translate_last_words();
    head[2] = n;
    pthread_mutex_lock(&mu);
    fwrite(head, 8, 3, f);
    fwrite(code, 4, b->ninsns, f);
    fwrite(w, 4, n, f);
    fflush(f);
    pthread_mutex_unlock(&mu);
}

static HvBlock *lookup(HvCpu *cpu, uint64_t va, uint64_t pa, uint32_t flags, const uint32_t *code, uint32_t avail) {
    HvMachine *m = cpu->m;
    uint32_t j = (uint32_t)((va >> 2) ^ (va >> 18) ^ ((uint64_t)flags << 13)) & ((1u << HV_JC_BITS) - 1);
    __typeof__(cpu->pc_state->jc[0]) *jc = cpu->pc_state->jc;
    HvBlock *c = jc[j].va == va ? jc[j].b : NULL;
    if (c && c->pa == pa && c->flags == flags && c->ram->page_gen[c->pg] == c->gen) return c;
    uint32_t h = hash_of(va, pa, flags);
    HvRam *ram = hv_ram_lookup(m, pa);
    uint64_t pg = (pa - ram->pa) >> HV_TLB_PAGE_BITS;
    uint32_t gen = __atomic_load_n(&ram->page_gen[pg], __ATOMIC_ACQUIRE);
    HvBlock *b = find(m, h, va, pa, flags, gen);
    if (b) {
        jc[j].va = va, jc[j].b = b;
        return b;
    }

    if (m->flush_pending) goto flush_unlocked;
    /* Writes to this page must take the slow path from now on so we notice them:
       before its code is read (so a write after that, or the IC IVAU after it,
       invalidates what we make), and without the lock (it may scan every CPU's
       TLB; two CPUs doing it at once is harmless). */
    if (!__atomic_load_n(&ram->code_pages[pg], __ATOMIC_RELAXED)) {
        __atomic_store_n(&ram->code_pages[pg], 1, __ATOMIC_SEQ_CST); /* before the TLBs: see the end of tlb_fill */
        m->stats.code_protects++;
        for (int i = 0; i < m->ncpus; i++) /* in every CPU's TLB, or a write from another CPU goes unnoticed */
            if (m->cpus[i]) hv_tlb_protect_code_page(m->cpus[i], ram, pg);
        gen = __atomic_load_n(&ram->page_gen[pg], __ATOMIC_ACQUIRE);
    }
    /* Translate without the lock, into this thread's buffer... */
    HvBlock nb = {.va = va, .pa = pa, .flags = flags, .gen = gen, .ram = ram, .pg = pg, .maker = cpu->index + 1, .jit_gen = __atomic_load_n(&m->jit_gen, __ATOMIC_SEQ_CST)};
    uint64_t t0 = m->stats_interval ? mach_absolute_time() : 0;
    uint8_t phase0 = cpu->pc_state ? cpu->pc_state->phase : 0;
    if (cpu->pc_state) __atomic_store_n(&cpu->pc_state->phase, HV_PHASE_TRANSLATE, __ATOMIC_RELAXED);
    int failed = hv_translate_emit(m, &nb, code, avail) != 0;
    if (cpu->pc_state) __atomic_store_n(&cpu->pc_state->phase, phase0, __ATOMIC_RELAXED);
    if (failed) goto flush_unlocked;
    if (m->stats_interval) {
        m->stats.translate_ticks += mach_absolute_time() - t0;
        m->stats.translated_insns += nb.ninsns;
    }
    /* ...and place it in the region (space is reserved atomically, so no lock either)... */
    size_t used0 = m->jit.used;
    if (hv_translate_place(m, &nb) != 0) { /* JIT region full: start over */
        pthread_mutex_lock(&m->tr_mu);
        m->stats.flush_full++;
        goto flush;
    }
    if (m->gen_checks && nb.jit_gen != __atomic_load_n(&m->jit_gen, __ATOMIC_SEQ_CST))
        fprintf(stderr, "hengevm: cpu %u translated across a flush: gen %u to %u, placed at rx+%llx, used %llx, active %d\n", cpu->index, nb.jit_gen, m->jit_gen, (unsigned long long)(nb.code - m->jit.base) * 4, (unsigned long long)m->jit.used * 4, cpu->pc_state->active);
    block_dump(&nb, code);
    /* ...then take the lock only to publish it. */
    if (pthread_mutex_trylock(&m->tr_mu) != 0) {
        uint64_t w0 = m->stats_interval ? mach_absolute_time() : 0;
        pthread_mutex_lock(&m->tr_mu);
        if (m->stats_interval) {
            m->stats.tr_waits++;
            m->stats.tr_wait_ticks += mach_absolute_time() - w0;
        }
    }
    b = find(m, h, va, pa, flags, gen); /* another CPU may have just made it: ours is dropped (its space stays used until a flush) */
    if (b) {
        pthread_mutex_unlock(&m->tr_mu);
        jc[j].va = va, jc[j].b = b;
        return b;
    }
    if ((uint32_t)m->nblocks >= m->max_blocks || m->flush_pending) {
        m->stats.flush_full++;
        goto flush;
    }
    b = &m->blocks[m->nblocks];
    *b = nb;
    m->stats.translations++;
    if (m->stats_interval) {
        m->stats.translated_words += m->jit.used - used0;
        static uint64_t seen_va[1 << 18], seen_vapa[1 << 18]; /* was this code translated before? (hashed, so approximate) */
        uint32_t hv = (uint32_t)(((va >> 2) ^ flags) * 0x9e3779b97f4a7c15ull >> 40);
        uint32_t hp = (uint32_t)((((va >> 2) ^ flags) * 0x9e3779b97f4a7c15ull ^ pa * 0xc2b2ae3d27d4eb4full) >> 40);
        if (seen_va[hv >> 6] >> (hv & 63) & 1) m->stats.retrans_va++;
        if (seen_vapa[hp >> 6] >> (hp & 63) & 1) m->stats.retrans_vapa++;
        seen_va[hv >> 6] |= 1ull << (hv & 63);
        seen_vapa[hp >> 6] |= 1ull << (hp & 63);
    }
    m->nblocks++;
    b->next = m->hash[h];
    __atomic_store_n(&m->hash[h], b, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&m->tr_mu);
    return b;
flush_unlocked:
    pthread_mutex_lock(&m->tr_mu);
flush:
    __atomic_store_n(&m->flush_pending, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&m->tr_mu);
    hv_machine_kick_all(m);
    return NULL;
}

/* WFE. xnu's spin loops arm the exclusive monitor on the lock word (LDAXR)
   and wait for it to change; leaving for the dispatcher every time made a
   waiting CPU spin through it (a hundred thousand times a second). Wait here
   instead: the host core sleeps in its own WFE on the host copy of the word,
   which wakes when another core writes it (or at the event stream, about every
   microsecond). A request for this CPU (an interrupt, a TLBI) ends the wait, and
   so does a short timeout, so its timer and devices are seen to. Without a
   monitor to watch it leaves at once, as before. HV_WFE_SPIN: always that. */
static uint32_t wfe(HvCpu *cpu) {
    static int spin = -1;
    if (spin < 0) spin = getenv("HV_WFE_SPIN") != NULL;
    uint64_t va = cpu->excl_addr;
    uint32_t size = cpu->excl_size;
    const HvTlbEntry *e = hv_tlb_ent(cpu, (va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), hv_tlb_table(cpu, cpu->el));
    uint64_t page = va & ~((1ull << HV_TLB_PAGE_BITS) - 1);
    if (spin || va == ~0ull || (size != 1 && size != 2 && size != 4 && size != 8) || (va & (size - 1)) ||
        e->read_tag == HV_TLB_INVALID || e->read_tag + HV_TLB_ADJ != page) {
        __atomic_store_n(&cpu->exit_req, 1, __ATOMIC_RELAXED); /* let another CPU run: the dispatcher's way */
        return 0;
    }
    const void *host = (const void *)(uintptr_t)(va + (uint64_t)e->addend);
    uint64_t want = cpu->excl_val[0] & (size == 8 ? ~0ull : (1ull << (8 * size)) - 1);
    /* The host's WFE wakes at its event stream (about every microsecond on the
       Mac); if it does not come back that soon (no event stream), spin instead. */
    static int sleep_ok = -1;
    if (sleep_ok < 0) {
        __asm__ volatile("sevl; wfe" ::: "memory"); /* clear the event register */
        uint64_t t0 = mach_absolute_time();
        __asm__ volatile("wfe" ::: "memory");
        sleep_ok = mach_absolute_time() - t0 < 24 * 20;
    }
    uint64_t deadline = mach_absolute_time() + 24 * 50; /* 50 us at 24 MHz */
    if (cpu->m->stats_interval) cpu->m->stats.wfe_waits++;
    for (;;) {
        uint64_t v;
        switch (size) { /* LDAXR arms the host monitor too, so the WFE below wakes on a write */
        case 1: { uint32_t w; __asm__ volatile("ldaxrb %w0, [%1]" : "=r"(w) : "r"(host) : "memory"); v = w; break; }
        case 2: { uint32_t w; __asm__ volatile("ldaxrh %w0, [%1]" : "=r"(w) : "r"(host) : "memory"); v = w; break; }
        case 4: { uint32_t w; __asm__ volatile("ldaxr %w0, [%1]" : "=r"(w) : "r"(host) : "memory"); v = w; break; }
        default: __asm__ volatile("ldaxr %0, [%1]" : "=r"(v) : "r"(host) : "memory"); break;
        }
        if (v != want || __atomic_load_n(&cpu->exit_req, __ATOMIC_RELAXED)) break;
        if (mach_absolute_time() >= deadline) {
            __atomic_store_n(&cpu->exit_req, 1, __ATOMIC_RELAXED);
            break;
        }
        if (sleep_ok) __asm__ volatile("wfe" ::: "memory");
        else __asm__ volatile("yield" ::: "memory");
    }
    __asm__ volatile("clrex" ::: "memory");
    return 0;
}

/* After an SVC or ERET, the block the CPU goes to next, if the dispatcher has
   nothing to do first: no request, interrupt or timer due, no flush, and the
   block in this CPU's indirect-branch cache for the new state. hv_helper then
   resumes there instead of leaving (a hundred thousand round trips a second). */
static void *fast_resume(HvCpu *cpu) {
    static int off = -1; /* HV_NO_FAST_RESUME: every exception and return goes through the dispatcher */
    if (off < 0) off = getenv("HV_NO_FAST_RESUME") != NULL;
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    if (off || !m->chain || m->trace || m->log >= 2 || !p || cpu->pc == m->stop_pc || cpu->pc == m->stop_pc2 || cpu->halted || cpu->off) { if (m->stats_interval) m->stats.fr_fail[0]++; return NULL; }
    if (__atomic_load_n(&cpu->exit_req, __ATOMIC_RELAXED) || __atomic_load_n(&m->flush_pending, __ATOMIC_RELAXED) ||
        __atomic_load_n(&m->stw_active, __ATOMIC_RELAXED)) {
        if (m->stats_interval) m->stats.fr_fail[1]++;
        return NULL;
    }
    if ((p->irq_line && !(cpu->daif & HV_PSTATE_I)) || (p->fiq_line && !(cpu->daif & HV_PSTATE_F))) { if (m->stats_interval) m->stats.fr_fail[2]++; return NULL; }
    if (cpu->timer_due && mach_absolute_time() >= cpu->timer_due) { if (m->stats_interval) m->stats.fr_fail[3]++; return NULL; }
    uint32_t flags = block_flags(cpu);
    int kernel = flags & HV_BLOCK_EL1;
    uint64_t va = cpu->pc;
    if ((int)((va >> 55) & 1) != kernel || (va & 3)) return NULL;
    uint64_t tag = va ^ cpu->ijc_salt[kernel];
    uint64_t *ent = p->ijc + (size_t)(flags & (HV_IJC_TABLES - 1)) * (HV_IJC_TABLE_BYTES / 8) + (size_t)hv_ijc_index(tag) * 2;
    void *code = ent[0] == tag ? (void *)(uintptr_t)ent[1] : ent[2] == tag ? (void *)(uintptr_t)ent[3] : NULL;
    if (!code) code = hv_ijc_resolve(cpu, va, flags); /* made already but not cached: cache it */
    if (code) {
        cpu->exit_reason = HV_EXIT_BRANCH;
        if (m->stats_interval) m->stats.fast_resumes++;
    } else if (m->stats_interval) m->stats.fr_fail[4]++;
    return code;
}

/* Blocks call this through hv_helper_entry. Returns 1 to leave the block. */
int hv_looks_host(uint64_t v);
/* What a host address is, by name: a value that turns up in a guest register has to be traced back
   to the object it belongs to, and a symbol name is the shortest way there. */
static const char *host_name(uint64_t v) {
    static char buf[320];
    Dl_info di;
    memset(&di, 0, sizeof di);
    if (dladdr((void *)(uintptr_t) v, &di) && di.dli_sname)
        snprintf(buf, sizeof buf, "%s+0x%llx", di.dli_sname,
                 (unsigned long long) (v - (uintptr_t) di.dli_saddr));
    else
        snprintf(buf, sizeof buf, "lib+0x%llx", 
                 (unsigned long long) (di.dli_fbase ? (uintptr_t) v - (uintptr_t) di.dli_fbase : 0));
    return buf;
}
static uint64_t reg_hist[65536]; /* --stats: MRS/MSR/SYS by op0:op1:CRn:CRm:op2; reads low, writes high 32 bits */

/* Could this TLBI (just done) change the mapping of pc's page? A page or range
   containing it, anything wider than one address space, or one address space's
   all when pc is a user address. */
static int tlbi_may_hit(HvCpu *cpu, uint32_t insn, uint64_t pc) {
    uint32_t op1 = (insn >> 16) & 7, crm = (insn >> 8) & 0xf, op2 = (insn >> 5) & 7;
    int rt = insn & 0x1f;
    uint64_t xt = rt == 31 ? 0 : cpu->x[rt];
    uint64_t page = pc & 0x00ffffffffffc000ull;
    if (op1 != 0) return 1;
    if ((crm == 1 || crm == 3 || crm == 7) && (op2 & 1)) { /* one page */
        uint64_t va = (uint64_t)((int64_t)((xt & 0xfffffffffffull) << 20) >> 8);
        return (va & 0x00ffffffffffc000ull) == page;
    }
    if ((crm == 2 || crm == 5 || crm == 6) && (op2 & 1)) { /* a range */
        uint32_t tg = (xt >> 46) & 3;
        int shift = tg == 1 ? 12 : tg == 2 ? 14 : tg == 3 ? 16 : 0;
        if (!shift) return 1;
        uint64_t pages = (((xt >> 39) & 31) + 1) << (5 * ((xt >> 44) & 3) + 1);
        uint64_t va = (uint64_t)((int64_t)((xt & 0x1fffffffffull) << (64 - 37)) >> (64 - 37 - shift)) & 0x00ffffffffffffffull;
        uint64_t len = pages << shift;
        return page + (1ull << HV_TLB_PAGE_BITS) > va && page < va + len;
    }
    if ((crm == 1 || crm == 3 || crm == 7) && op2 == 2) return !((pc >> 55) & 1); /* ASIDE1: user pages */
    return 1; /* VMALLE1 and the rest */
}

static uint32_t helper(HvCpu *cpu, const HvHelperInfo *info_in);

uint32_t hv_helper(HvCpu *cpu, const HvHelperInfo *info_in) {
    HvPerCpu *pcs = cpu->pc_state;
    if (cpu->m->log >= 2) {
        /* The ring the data-abort dump reads: who called, with what. */
        uint32_t i = cpu->helper_hist_idx++ & 63;
        cpu->helper_hist[i].pc = info_in->pc;
        cpu->helper_hist[i].op = info_in->op & 0xff;
        cpu->helper_hist[i].x4 = cpu->x[4];
        cpu->helper_hist[i].x16 = cpu->x[16];
        cpu->helper_hist[i].x30 = cpu->x[30];
        cpu->helper_hist[i].x0 = cpu->x[0];
    }
    if (pcs) __atomic_store_n(&pcs->phase, HV_PHASE_HELPER, __ATOMIC_RELAXED);
    /* Whether a helper is what moves a register: the guest's own instructions are the only other
       writers, and those are known. A value that leaves the guest's own address space is the
       interesting case, so it is the one that gets printed. */
    const uint64_t x16_before = cpu->x[16], x4_before = cpu->x[4];
    /* The first helpers of a boot: what the save point saw, before any C code ran. A register
       already wrong here was written by the block or by the thunk, not by the helper. */
    if (cpu->m->log >= 2 && cpu->icount < 40)
        fprintf(stderr, "hengevm: helper in: pc %llx op %llu insn %08x x16 %llx x4 %llx\n",
                (unsigned long long) info_in->pc, (unsigned long long) (info_in->op & 0xff),
                info_in->insn, (unsigned long long) x16_before, (unsigned long long) x4_before);
    uint32_t r = helper(cpu, info_in);
    if (cpu->m->log >= 2) {
        if (cpu->x[16] != x16_before && cpu->x[16] > 0x100000000ull)
            fprintf(stderr, "hengevm: helper x16: pc %llx op %llu insn %08x %llx -> %llx\n",
                    (unsigned long long) info_in->pc, (unsigned long long) (info_in->op & 0xff),
                    info_in->insn, (unsigned long long) x16_before, (unsigned long long) cpu->x[16]);
        if (cpu->x[4] != x4_before && cpu->x[4] > 0x100000000ull)
            fprintf(stderr, "hengevm: helper x4: pc %llx op %llu insn %08x %llx -> %llx\n",
                    (unsigned long long) info_in->pc, (unsigned long long) (info_in->op & 0xff),
                    info_in->insn, (unsigned long long) x4_before, (unsigned long long) cpu->x[4]);
    }
    if (pcs) __atomic_store_n(&pcs->phase, HV_PHASE_RUN, __ATOMIC_RELAXED);
    /* An exception just taken (an abort, an undefined instruction, a trap): its
       vector's block, as after an SVC. */
    if (r && cpu->exit_reason == HV_EXIT_EXCEPTION) {
        void *code = fast_resume(cpu);
        if (code) {
            cpu->helper_ret = (uint64_t)(uintptr_t)code;
            return 0;
        }
    }
    return r;
}

static uint32_t helper(HvCpu *cpu, const HvHelperInfo *info_in) {
    HvHelperInfo info_copy = *info_in;
    info_copy.op &= 0xff; /* the rest: hv_helper_thunk's refill flags */
    const HvHelperInfo *info = &info_copy;
    if (info->op < 12 && cpu->m->stats_interval) cpu->m->stats.helper[info->op]++;
    if (cpu->m->stats_interval && (info->op == HV_OP_MRS || info->op == HV_OP_MSR || info->op == HV_OP_SYS))
        reg_hist[(info->insn >> 5) & 0xffff] += info->op == HV_OP_MRS ? 1 : 1ull << 32;
    switch (info->op) {
    case HV_OP_MEM:
        cpu->m->stat_slow_mem++;
        if (cpu->m->stats_interval) { /* --stats: which kinds of access take the slow path */
            HvMemOp op;
            if (hv_mem_decode(info->insn, &op) == 0 && op.kind < 12) cpu->m->stats.slow_kind[op.kind]++;
        }
        return hv_mem_emulate(cpu, info->insn, info->pc);
    case HV_OP_MRS:
        return hv_sysreg_mrs(cpu, info->insn, info->pc);
    case HV_OP_MSR:
        return hv_sysreg_msr(cpu, info->insn, info->pc);
    case HV_OP_SYS: {
        uint32_t r = hv_sysreg_sys(cpu, info->insn, info->pc);
        if (!r && ((info->insn >> 12) & 0xf) == 8 && tlbi_may_hit(cpu, info->insn, info->pc)) {
            cpu->pc = info->pc + 4; /* the block carries on otherwise: leave, to fetch the rest anew */
            return 1;
        }
        return r;
    }
    case HV_OP_EXCEPTION: {
        return hv_exception_insn(cpu, info->insn, info->pc); /* SVC: see hv_helper */
    }
    case HV_OP_ERET: {
        hv_eret(cpu);
        void *code = fast_resume(cpu);
        if (code) {
            cpu->helper_ret = (uint64_t)(uintptr_t)code;
            return 0;
        }
        return 1;
    }
    case HV_OP_FPTRAP:
        hv_raise_sync(cpu, HV_EC_FP_ACCESS, 1u << 24 | 0xeu << 20, 0, info->pc);
        return 1;
    case HV_OP_CLREX:
        cpu->excl_addr = ~0ull;
        return 0;
    case HV_OP_WFI:
        cpu->pc = info->pc + 4;
        cpu->exit_reason = HV_EXIT_WFI;
        return 1;
    case HV_OP_WFE:
        return wfe(cpu);
    case HV_OP_UNDEF:
    default:
        hv_raise_undef(cpu, info->pc);
        return 1;
    }
}

/* Debugging: EL0 is running a block translated from kernel code. Which block, and how did we get here? */
void hv_debug_el0_in_kernel(HvCpu *cpu, const char *what) {
    {
        {
            HvMachine *m = cpu->m;
            uint32_t *host = (uint32_t *)(uintptr_t)cpu->helper_ret; /* the block's blr into hv_helper_entry */
            int lo = 0, hi = m->nblocks - 1;
            while (lo < hi) {
                int mid = (lo + hi + 1) / 2;
                if (m->blocks[mid].code <= host) lo = mid;
                else hi = mid - 1;
            }
            HvBlock *b = m->nblocks ? &m->blocks[lo] : NULL;
            fprintf(stderr, "hengevm: el0 runs kernel code: block %llx flags %u (index %d of %d), flushes %llu, last stub %llx, "
                            "miss site %llx\n  recent:", b ? (unsigned long long)b->va : 0ull, b ? b->flags : 0, lo, m->nblocks,
                    (unsigned long long)m->stats.block_flushes, (unsigned long long)cpu->last_stub,
                    (unsigned long long)cpu->miss_site);
            for (int k = 15; k >= 0; k--) fprintf(stderr, " %llx", (unsigned long long)m->stats.recent[(m->stats.nrecent - 1 - k) & 15]);
            fprintf(stderr, "\n  (%s) el changes, oldest first:\n", what);
            for (int k = 15; k >= 0; k--) {
                uint32_t i = (m->stats.nelchg - 1 - k) & 15;
                if (m->stats.elchg[i].kind == 9)
                    fprintf(stderr, "    eret at %llx from el%u to %llx spsr %llx\n", (unsigned long long)m->stats.elchg[i].from,
                            m->stats.elchg[i].el, (unsigned long long)m->stats.elchg[i].to, (unsigned long long)m->stats.elchg[i].spsr);
                else
                    fprintf(stderr, "    exception type %u esr %llx from el%u pc %llx (spsr %llx)\n", m->stats.elchg[i].kind,
                            (unsigned long long)m->stats.elchg[i].to, m->stats.elchg[i].el,
                            (unsigned long long)m->stats.elchg[i].from, (unsigned long long)m->stats.elchg[i].spsr);
            }
        }
    }
}

/* WFI: sleep until an interrupt is pending (masked or not) or the next timer
   event, at most 10 ms. WFI may complete early, so the guest just runs again
   if nothing happened. */
static int wait_for_interrupt(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    uint64_t deadline = m->tick ? m->tick(m, cpu) : 0;
    HvPerCpu *p = cpu->pc_state;
    if (p->irq_line || p->fiq_line) return 0;
    if (!m->tick) return -1; /* nothing could ever wake us */
    uint64_t now = mach_absolute_time();
    uint64_t wait_ticks = deadline && deadline > now ? deadline - now : 240000; /* the counter runs at 24 MHz */
    if (wait_ticks > 240000) wait_ticks = 240000;
    uint64_t t0 = mach_absolute_time();
    /* Sleep until the next timer event or until a device thread has work for us. */
    hv_big_unlock(m); /* let the other CPUs run while this one sleeps */
    park(cpu);
    /* A thread on another CPU often has something for this one microseconds later
       (a pipe, a Mach message: macOS is made of these), and waking a sleeping host
       thread takes far longer than that: watch for it a little first. HV_WFI_SPIN: microseconds. */
    int spin_us = hv_knob[HV_K_WFI_SPIN];
    if (spin_us) {
        static int sleep_ok = -1; /* does the host's WFE come back soon by itself? (see wfe()) */
        if (sleep_ok < 0) {
            __asm__ volatile("sevl; wfe" ::: "memory");
            uint64_t t = mach_absolute_time();
            __asm__ volatile("wfe" ::: "memory");
            sleep_ok = mach_absolute_time() - t < 24 * 20;
        }
        uint64_t until = mach_absolute_time() + (uint64_t)spin_us * 24;
        for (;;) {
            uint32_t v;
            __asm__ volatile("ldaxr %w0, [%1]" : "=r"(v) : "r"(&p->wake_pending) : "memory"); /* a store to it wakes the WFE */
            if (v || mach_absolute_time() >= until) break;
            if (sleep_ok) __asm__ volatile("wfe" ::: "memory");
            else __asm__ volatile("yield" ::: "memory");
        }
        __asm__ volatile("clrex" ::: "memory");
    }
    pthread_mutex_lock(&p->wake_mu);
    if (!p->wake_pending) {
        struct timespec ts = {(time_t)(wait_ticks / 24000000), (long)(wait_ticks % 24000000) * 1000 / 24};
        pthread_cond_timedwait_relative_np(&p->wake_cv, &p->wake_mu, &ts);
    }
    p->wake_pending = 0;
    pthread_mutex_unlock(&p->wake_mu);
    unpark(cpu);
    hv_big_lock(m);
    m->stats.wfi++;
    m->stats.wfi_ticks += mach_absolute_time() - t0;
    m->tick(m, cpu);
    return 0;
}

/* --profile: which guest blocks the sampled host PCs were in. Blocks are laid
   out in the JIT region in the order they were made, so a binary search over
   m->blocks finds the one holding a PC. */
static void print_profile(HvMachine *m) {
    enum { SLOTS = 4096 };
    static struct { uint64_t va; uint32_t flags, n, ninsns; } top[SLOTS];
    memset(top, 0, sizeof top);
    uint32_t total = 0, outside = 0;
    static struct { const char *name; uint32_t n; } fn[64];
    static struct { uint32_t off, n; } run_off[256];
    int nfn = 0, nrun = 0;
    uint32_t head = __atomic_load_n(&m->prof_head, __ATOMIC_ACQUIRE);
    if (head - m->prof_tail > HV_PROF_RING) m->prof_tail = head - HV_PROF_RING;
    for (; m->prof_tail != head; m->prof_tail++) {
        uint32_t *pc = (uint32_t *)(uintptr_t)m->prof[m->prof_tail & (HV_PROF_RING - 1)];
        total++;
        if (pc < m->jit.base || pc >= m->jit.base + m->jit.used || !m->nblocks) {
            outside++;
            Dl_info di;
            const char *name = dladdr(pc, &di) && di.dli_sname ? di.dli_sname : "?";
            static const char *detail;
            if (!detail) detail = getenv("HV_PROF_FN") ? getenv("HV_PROF_FN") : "run";
            if (!strcmp(name, detail)) { /* per-offset detail for one function (HV_PROF_FN, default the dispatcher loop) */
                uint32_t off = (uint32_t)((uint8_t *)pc - (uint8_t *)di.dli_saddr);
                int r = 0;
                while (r < nrun && run_off[r].off != off) r++;
                if (r == nrun && nrun < 256) { run_off[nrun].off = off; run_off[nrun++].n = 0; }
                if (r < nrun) run_off[r].n++;
            }
            int k = 0;
            while (k < nfn && strcmp(fn[k].name, name)) k++;
            if (k == nfn && nfn < 64) { fn[nfn].name = name; fn[nfn++].n = 0; }
            if (k < nfn) fn[k].n++;
            continue;
        }
        int lo = 0, hi = m->nblocks - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (m->blocks[mid].code <= pc) lo = mid;
            else hi = mid - 1;
        }
        HvBlock *b = &m->blocks[lo];
        uint32_t h = (uint32_t)((b->va >> 2) ^ b->flags) & (SLOTS - 1);
        int probes = 0;
        while (top[h].n && (top[h].va != b->va || top[h].flags != b->flags) && ++probes < 64) h = (h + 1) & (SLOTS - 1);
        if (probes == 64) continue; /* table full: the block is too cold to make the list anyway */
        top[h].va = b->va;
        top[h].flags = b->flags;
        top[h].ninsns = b->ninsns;
        top[h].n++;
    }
    if (!total) return;
    fprintf(stderr, "  profile: %u samples, %.0f%% outside translated code:", total, 100.0 * outside / total);
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < nfn; i++)
            if (fn[i].n && (best < 0 || fn[i].n > fn[best].n)) best = i;
        if (best < 0) break;
        fprintf(stderr, " %s %.1f%%", fn[best].name, 100.0 * fn[best].n / total);
        fn[best].n = 0;
    }
    fprintf(stderr, "\n  %s+:", getenv("HV_PROF_FN") ? getenv("HV_PROF_FN") : "run");
    for (int k = 0; k < 12; k++) {
        int best = -1;
        for (int i = 0; i < nrun; i++)
            if (run_off[i].n && (best < 0 || run_off[i].n > run_off[best].n)) best = i;
        if (best < 0) break;
        fprintf(stderr, " %#x:%.1f%%", run_off[best].off, 100.0 * run_off[best].n / total);
        run_off[best].n = 0;
    }
    fprintf(stderr, "\n");
    for (int k = 0; k < 12; k++) {
        int best = -1;
        for (int i = 0; i < SLOTS; i++)
            if (top[i].n && (best < 0 || top[i].n > top[best].n)) best = i;
        if (best < 0 || top[best].n * 200 < total) break;
        fprintf(stderr, "    %5.1f%%  %llx el%u (%u insns)\n", 100.0 * top[best].n / total,
                (unsigned long long)top[best].va, top[best].flags & HV_BLOCK_EL1, top[best].ninsns);
        top[best].n = 0;
    }
}

static void print_stats(HvCpu *cpu, int force) {
    static uint64_t last_t, start_t;
    /* This uses floating point, and the host FP registers hold the guest's state. */
    hv_fp_leave(cpu);
    uint64_t zero = 0;
    __asm__ volatile("msr fpcr, %0" ::"r"(zero));
    static HvStats last;
    HvMachine *m = cpu->m;
    uint64_t now = mach_absolute_time();
    if (!start_t) start_t = last_t = now;
    if (!force && now - last_t < (uint64_t)m->stats_interval * 24000000ull) {
        hv_fp_enter(cpu);
        return;
    }
    double dt = (double)(now - last_t) / 24e6;
    HvStats *s = &m->stats;
#define RATE(f) ((double)(s->f - last.f) / dt)
    fprintf(stderr,
            "stats %5.0fs: blocks %.2fM/s, translated %.0f/s (again: va %.0f%% va+pa %.0f%%; %.0f%% of time, %.1f insns, %.1fx), slow mem %.0fK/s, mrs %.0fK/s msr %.0fK/s sys %.0fK/s, "
            "exc %.0f/s irq %.0f/s, tlb flush %.0f/s, fetch walks %.0fK/s, wfi %.0f/s (%.0f%% idle), flushes %.0f/s (smc %.0f ic %.0f full %.0f), blocks held %d (%.0f MB, %llu full flushes), chained %.0f/s ijc fills %.0fK/s resets %.0f/s, el%u pc %llx\n",
            (double)(now - start_t) / 24e6, RATE(blocks) / 1e6, RATE(translations), 100.0 * RATE(retrans_va) / (RATE(translations) + 1e-9), 100.0 * RATE(retrans_vapa) / (RATE(translations) + 1e-9), 100.0 * (double)(s->translate_ticks - last.translate_ticks) / (double)(now - last_t), RATE(translated_insns) / (RATE(translations) + 1e-9), RATE(translated_words) / (RATE(translated_insns) + 1e-9), RATE(helper[HV_OP_MEM]) / 1e3,
            RATE(helper[HV_OP_MRS]) / 1e3, RATE(helper[HV_OP_MSR]) / 1e3, RATE(helper[HV_OP_SYS]) / 1e3,
            RATE(exceptions), RATE(interrupts), RATE(tlb_flushes), RATE(fetch_walks) / 1e3, RATE(wfi),
            100.0 * (double)(s->wfi_ticks - last.wfi_ticks) / (double)(now - last_t), RATE(block_flushes), RATE(flush_smc),
            RATE(flush_ic), RATE(flush_full), m->nblocks, (double)m->jit.used * 4 / (1 << 20), (unsigned long long)s->flush_full, RATE(chained), RATE(ijc_fills) / 1e3, RATE(ijc_resets), cpu->el,
            (unsigned long long)cpu->pc);
    {
        double tot = 0;
        for (int k = 0; k < 6; k++) tot += RATE(why[k]);
        /* How much of the interval the CPU thread actually ran: below 100% means the host took it away. */
        static double last_busy;
        thread_basic_info_data_t ti;
        mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
        mach_port_t self = mach_thread_self();
        if (thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&ti, &cnt) == KERN_SUCCESS) {
            double busy = ti.user_time.seconds + ti.user_time.microseconds / 1e6 + ti.system_time.seconds +
                          ti.system_time.microseconds / 1e6;
            fprintf(stderr, "  cpu thread ran %.0f%% of the interval\n", 100 * (busy - last_busy) / dt);
            if (m->ncpus > 1) {
                fprintf(stderr, "  cpus:");
                for (int i = 0; i < m->ncpus; i++)
                    if (m->cpus[i])
                        fprintf(stderr, " [%d] %s el%u pc %llx", i, m->cpus[i]->off ? "off" : "on", m->cpus[i]->el,
                                (unsigned long long)m->cpus[i]->pc);
                fprintf(stderr, "\n");
            }
            last_busy = busy;
        }
        mach_port_deallocate(mach_task_self(), self);
        fprintf(stderr, "  whole-TLB flushes/s: TLBI all here %.0f, TLBI all shared %.0f, SCTLR %.0f, TTBR1 %.0f, TCR %.0f, MAIR %.0f, range of all %.0f, requests overflowed %.0f\n",
                RATE(flush_why[0]), RATE(flush_why[1]), RATE(flush_why[2]), RATE(flush_why[3]), RATE(flush_why[4]), RATE(flush_why[5]),
                RATE(flush_why[6]), RATE(flush_why[7]));
        fprintf(stderr, "  TLBI all here/s by time since a TTBR0 change: <2us %.0f, <20us %.0f, <200us %.0f, later %.0f; left out %.0f (%.0f of them late); at",
                RATE(flush_after_switch[0]), RATE(flush_after_switch[1]), RATE(flush_after_switch[2]), RATE(flush_after_switch[3]), RATE(flush_skipped),
                RATE(flush_skipped_late));
        for (int i = 0; i < 8 && s->flush_pc[i].pc; i++) fprintf(stderr, " %llx x%llu", (unsigned long long)s->flush_pc[i].pc, (unsigned long long)s->flush_pc[i].n);
        fprintf(stderr, "\n");
        if (s->flush_checked)
            fprintf(stderr, "  switch-flush checks/s: %.0f, entries walked %.0f, found out of date %.0f (flushed), access flag only %.0f\n",
                    RATE(flush_checked), RATE(flush_check_entries), RATE(flush_check_bad), RATE(flush_check_af));
        fprintf(stderr, "  cache drops/s: full %.0f, ASID %.0f, user VA %.0f, kernel VA %.0f, code %.0f (whole user half %.0f, filter full %.0f, big user range %.0f)\n", RATE(reset_why[0]),
                RATE(reset_why[1]), RATE(reset_why[2]), RATE(reset_why[3]), RATE(reset_why[4]), RATE(reset_why[5]), RATE(reset_why[6]), RATE(reset_why[7]));
        { /* block hash health: chain lengths and how much of it is stale */
            uint64_t used = 0, stale = 0, maxlen = 0, sumsq = 0;
            uint32_t worst = 0;
            for (uint32_t h = 0; h < HV_BLOCK_HASH; h++) {
                uint64_t len = 0;
                for (HvBlock *b = m->hash[h]; b; b = b->next) {
                    len++;
                    if (b->ram->page_gen[b->pg] != b->gen) stale++;
                }
                if (len) used++;
                sumsq += len * len;
                if (len > maxlen) maxlen = len, worst = h;
            }
            fprintf(stderr, "  block hash: %llu buckets used, %llu stale blocks, max chain %llu, mean walk %.1f\n",
                    (unsigned long long)used, (unsigned long long)stale, (unsigned long long)maxlen,
                    m->nblocks ? (double)sumsq / m->nblocks : 0.0);
            int k = 0;
            for (HvBlock *b = m->hash[worst]; b && k < 6; b = b->next, k++)
                fprintf(stderr, "    worst: va %llx pa %llx flags %x gen %u/%u\n", (unsigned long long)b->va,
                        (unsigned long long)b->pa, b->flags, b->gen, b->ram->page_gen[b->pg]);
        }
        {
            double tw = RATE(words_inline) + RATE(words_ool) + RATE(words_pool) + 1e-9;
            fprintf(stderr, "  code words: inline %.0f%% out of line %.0f%% literals %.0f%%\n", 100 * RATE(words_inline) / tw,
                    100 * RATE(words_ool) / tw, 100 * RATE(words_pool) / tw);
        }
        fprintf(stderr, "  new code pages %.0f/s; waits: tlbi acks %.0f/s %.1f%%, device lock %.0f/s %.1f%%, translation lock %.0f/s %.1f%% (of one CPU)\n",
                RATE(code_protects), RATE(syncs), RATE(sync_ticks) / 240000.0, RATE(dev_waits), RATE(dev_wait_ticks) / 240000.0,
                RATE(tr_waits), RATE(tr_wait_ticks) / 240000.0);
        {
            double l = RATE(syncs) + 1e-9;
            fprintf(stderr, "  tlbi ack waits by length/s: <10us %.0f (%.1f%%), <100us %.0f (%.1f%%), <1ms %.0f (%.1f%%), longer %.0f (%.1f%%); "
                            "at 100 us the CPU waited for was: off a core %.0f, on one %.0f (mid walk %.0f, mid cache fill %.0f)\n",
                    RATE(sync_len[0]), RATE(sync_len_ticks[0]) / 240000.0, RATE(sync_len[1]), RATE(sync_len_ticks[1]) / 240000.0,
                    RATE(sync_len[2]), RATE(sync_len_ticks[2]) / 240000.0, RATE(sync_len[3]), RATE(sync_len_ticks[3]) / 240000.0,
                    RATE(sync_probe[0]), RATE(sync_probe[1]), RATE(sync_probe[2]), RATE(sync_probe[3]));
            fprintf(stderr, "  at 1 ms the CPU waited for was/s: running %.1f, translating %.1f, in a helper %.1f, waiting for acks %.1f, at the flush barrier %.1f, off a core %.1f\n",
                    RATE(sync_phase[0]), RATE(sync_phase[1]), RATE(sync_phase[2]), RATE(sync_phase[3]), RATE(sync_phase[4]), RATE(sync_phase[5]));
            (void)l;
        }
        fprintf(stderr, "  light lookups failed/s: odd %.0f, filters full %.0f, no mapping %.0f, no block yet %.0f; walked %.0fK/s\n",
                RATE(light_fail[0]), RATE(light_fail[1]), RATE(light_fail[2]), RATE(light_fail[3]), RATE(light_walks) / 1e3);
        fprintf(stderr, "  big user-range TLBIs/s: %.0f TLB only in the range, %.0f with no cached targets to drop; big kernel ranges %.0f\n",
                RATE(tlb_range_flushes), RATE(range_spared), RATE(kernel_ranges));
        fprintf(stderr, "  whole branch-cache drops/s: user half %.0f, kernel half %.0f, a process's by TLBI ASID %.0f, by a range of 64 MB+ %.0f\n",
                RATE(ijc_reset_kind[0]), RATE(ijc_reset_kind[1]), RATE(ijc_reset_kind[2]), RATE(ijc_reset_kind[3]));
        fprintf(stderr, "  code changes/s: no cached target into the page %.0f, that VA page's targets dropped %.0f, all dropped %.0f\n",
                RATE(code_ijc[0]), RATE(code_ijc[1]), RATE(code_ijc[2]));
        fprintf(stderr, "  TLBIs left to other CPUs/s: raced %.0f, every address space's cached targets %.0f, big range %.0f, ASID or all %.0f\n",
                RATE(posted_kind[0]), RATE(posted_kind[3]), RATE(posted_kind[1]), RATE(posted_kind[2]));
        fprintf(stderr, "  remote drop races/s: walking %.0f, filling %.0f, during %.0f; walks of other pages let through %.0f\n", RATE(race_kind[0]), RATE(race_kind[1]), RATE(race_kind[2]),
                RATE(race_other_page));
        {
            static const char *kn[16] = {"copy", "pcrel", "branch", "branch-reg", "eret", "literal", "memory", "prefetch",
                                         "exception", "sysreg", "sys", "barrier", "wait", "pac", "undef", "?"};
            uint64_t ti = 0, tw = 0;
            for (int k = 0; k < 16; k++) ti += s->kind_insns[k] - last.kind_insns[k], tw += s->kind_words[k] - last.kind_words[k];
            if (ti) {
                fprintf(stderr, "  inline words per insn %.2f:", (double)tw / ti);
                for (int k = 0; k < 16; k++) {
                    uint64_t i = s->kind_insns[k] - last.kind_insns[k], w = s->kind_words[k] - last.kind_words[k];
                    if (i * 200 >= ti) fprintf(stderr, " %s %.0f%%x%.1f", kn[k], 100.0 * i / ti, (double)w / i);
                }
                fprintf(stderr, "\n");
            }
        }
        fprintf(stderr, "  user light fills: shared region %.0fK/s, elsewhere %.0fK/s\n", RATE(light_region[0]) / 1e3, RATE(light_region[1]) / 1e3);
        fprintf(stderr, "  code-page protects/s: none %.0f, one entry %.0f, whole-TLB scans %.0f\n", RATE(protect_kind[0]),
                RATE(protect_kind[1]), RATE(protect_kind[2]));
        fprintf(stderr, "  fast resume refused/s: state %.0f, exit request %.0f, interrupt %.0f, timer %.0f, not cached %.0f\n", RATE(fr_fail[0]),
                RATE(fr_fail[1]), RATE(fr_fail[2]), RATE(fr_fail[3]), RATE(fr_fail[4]));
        fprintf(stderr, "  WFE waits in place %.0f/s; exits chained as placed %.0f/s; SVC/ERET resumed in place %.0f/s\n", RATE(wfe_waits),
                RATE(chained_early), RATE(fast_resumes));
        fprintf(stderr, "  exit requests/s: TLBI posts %.0f, wakes %.0f (of them device work %.0f), kick all %.0f, own %.0f\n",
                RATE(kick_src[0]), RATE(kick_src[1]), RATE(kick_src[3]), RATE(kick_src[2]), RATE(kick_src[4]));
        fprintf(stderr, "  light refills %.0fK/s, light branch-cache fills %.0fK/s, chained to other pages %.0f/s\n", RATE(refills) / 1e3,
                RATE(ijc_light) / 1e3, RATE(xchained));
        fprintf(stderr, "  process switches: %.0f/s saved, %.0f/s restored, %.0f/s dropped by a TLBI (%.0f entries each)\n", RATE(stash_saves), RATE(stash_hits), RATE(stash_stale),
                s->stash_hits - last.stash_hits ? (double)(s->stash_entries - last.stash_entries) / (s->stash_hits - last.stash_hits) : 0.0);
        fprintf(stderr, "  tlb misses K/s: el0 empty %.0f el0 conflict %.0f el1 empty %.0f el1 conflict %.0f\n",
                RATE(tlb_miss_kind[0]) / 1e3, RATE(tlb_miss_kind[1]) / 1e3, RATE(tlb_miss_kind[2]) / 1e3, RATE(tlb_miss_kind[3]) / 1e3);
        fprintf(stderr, "  slow mem causes K/s: mmio %.0f code-page write %.0f page end %.0f tlb miss %.0f other %.0f entry-but-tag %.0f\n",
                RATE(slow_why[0]) / 1e3, RATE(slow_why[1]) / 1e3, RATE(slow_why[2]) / 1e3, RATE(slow_why[3]) / 1e3,
                RATE(slow_why[4]) / 1e3, RATE(slow_why[5]) / 1e3);
        fprintf(stderr, "  slow mem on pages mapped under 16 KB: %.0fK/s; at (MB, el)", RATE(small_pages) / 1e3);
        for (int i = 0; i < 8 && s->small_at[i].mb; i++)
            fprintf(stderr, " %llx/%d x%llu", (unsigned long long)(s->small_at[i].mb >> 1), (int)(s->small_at[i].mb & 1), (unsigned long long)s->small_at[i].n);
        fprintf(stderr, "\n");
        fprintf(stderr, "  slow mem K/s: single %.0f pair %.0f literal %.0f ldra %.0f ordered %.0f excl %.0f cas %.0f casp %.0f atomic %.0f vec %.0f/%.0f\n",
                RATE(slow_kind[0]) / 1e3, RATE(slow_kind[1]) / 1e3, RATE(slow_kind[2]) / 1e3, RATE(slow_kind[3]) / 1e3,
                RATE(slow_kind[4]) / 1e3, RATE(slow_kind[5]) / 1e3, RATE(slow_kind[6]) / 1e3, RATE(slow_kind[7]) / 1e3,
                RATE(slow_kind[8]) / 1e3, RATE(slow_kind[9]) / 1e3, RATE(slow_kind[10]) / 1e3);
        fprintf(stderr, "  ijc fills into: empty %.0fK/s, live other salt %.0fK/s, dead salt %.0fK/s, same salt %.0fK/s\n",
                RATE(fill_kind[0]) / 1e3, RATE(fill_kind[1]) / 1e3, RATE(fill_kind[3]) / 1e3, RATE(fill_kind[2]) / 1e3);
        fprintf(stderr, "  light fills into: empty %.0fK/s, live other salt %.0fK/s, dead salt %.0fK/s, same salt %.0fK/s\n",
                RATE(light_kind[0]) / 1e3, RATE(light_kind[1]) / 1e3, RATE(light_kind[3]) / 1e3, RATE(light_kind[2]) / 1e3);
        fprintf(stderr, "  misses at: RET %.0fK/s, BR/BLR %.0fK/s, B/BL %.0fK/s, conditional %.0fK/s, MSR %.0fK/s, other %.0fK/s, unknown %.0fK/s\n",
                RATE(miss_kind[0]) / 1e3, RATE(miss_kind[1]) / 1e3, RATE(miss_kind[2]) / 1e3, RATE(miss_kind[3]) / 1e3,
                RATE(miss_kind[4]) / 1e3, RATE(miss_kind[5]) / 1e3, RATE(miss_kind[6]) / 1e3);
        fprintf(stderr, "  entries %.2fM/s: chain fill %.0f%%, kick %.0f%%, exception/sysreg %.0f%%, interrupt %.0f%%, cached target %.0f%%, cache miss %.0f%%\n",
                tot / 1e6, 100 * RATE(why[0]) / tot, 100 * RATE(why[1]) / tot, 100 * RATE(why[2]) / tot,
                100 * RATE(why[3]) / tot, 100 * RATE(why[4]) / tot, 100 * RATE(why[5]) / tot);
    }
#undef RATE
    if (m->prof) print_profile(m);
    /* The five busiest system registers or SYS operations (reads + writes) this interval. */
    for (int k = 0; k < 14; k++) {
        uint64_t best = 0;
        uint32_t bi = 0;
        for (uint32_t r = 0; r < 65536; r++) {
            uint64_t n = (reg_hist[r] & 0xffffffff) + (reg_hist[r] >> 32);
            if (n > best) best = n, bi = r;
        }
        if (!best) break;
        fprintf(stderr, "   S%u_%u_C%u_C%u_%u: %.0f reads %.0f writes/sys per s\n", bi >> 14, (bi >> 11) & 7, (bi >> 7) & 15,
                (bi >> 3) & 15, bi & 7, (double)(reg_hist[bi] & 0xffffffff) / dt, (double)(reg_hist[bi] >> 32) / dt);
        reg_hist[bi] = 0;
    }
    memset(reg_hist, 0, sizeof reg_hist);
    last = *s;
    last_t = now;
    hv_fp_enter(cpu);
}

/* Fill the slot of the chainable exit a block just left through, so it jumps
   straight to b next time. Chains never need undoing: both blocks are in the
   same 16 KB page, so when that page's code is invalidated the stale blocks are
   only reachable from each other, and the exit_req check before every chained
   jump gets a block that just wrote its own page back to the dispatcher. */
static void chain(HvMachine *m, uint64_t *slot, HvBlock *b) {
    uint32_t idx = (uint32_t)(slot - m->slots);
    if (m->log >= 2 && hv_self) hv_pc_hist_note_va(hv_self->index, b->va);
    if (m->gen_checks && (b->jit_gen != m->jit_gen || (m->slot_site[idx] && (uint64_t)(m->slot_site[idx] - m->jit.base) >= m->jit.used)))
        fprintf(stderr, "hengevm: chain slot %u site %p to rx+%llx va %llx: block gen %u now %u\n", idx, (void *)m->slot_site[idx], (unsigned long long)(b->code - m->jit.base) * 4, (unsigned long long)b->va, b->jit_gen, m->jit_gen);
    uint64_t xa = m->slot_xa[idx];
    if (xa) {
        /* From another page: only to the block in the physical page the exit's
           check names, and not a stale one (see hv_invalidate_code). */
        uint64_t page_pa = b->pa & ~((1ull << HV_TLB_PAGE_BITS) - 1);
        uint64_t host = (uint64_t)(uintptr_t)(b->ram->host + (page_pa - b->ram->pa));
        if (host - (b->va & ~((1ull << HV_TLB_PAGE_BITS) - 1)) != xa) return;
        pthread_mutex_lock(&m->tr_mu);
        if (__atomic_load_n(&b->ram->page_gen[b->pg], __ATOMIC_ACQUIRE) != b->gen || m->slot_xpa[idx]) {
            pthread_mutex_unlock(&m->tr_mu);
            return;
        }
        m->slot_xpa[idx] = page_pa | 1;
        m->slot_xnext[idx] = b->ram->xhead[b->pg]; /* on the target page's list (hv_invalidate_code) */
        b->ram->xhead[b->pg] = idx + 1;
        m->nxchained++;
        __atomic_store_n(slot, (uint64_t)(uintptr_t)b->code, __ATOMIC_RELEASE);
        uint32_t *site = m->slot_site[idx];
        int64_t off = site ? b->code - site : 0;
        if (site && off >= -(1 << 25) && off < (1 << 25)) {
            uint32_t insn = 0x14000000u | (uint32_t)(off & 0x3ffffff);
            hv_jit_write(&m->jit, site, &insn, 4);
            hv_jit_finish_branch(&m->jit, site); /* B to B; the stub follows the slot meanwhile */
        }
        m->stats.chained++;
        if (m->stats_interval) m->stats.xchained++;
        pthread_mutex_unlock(&m->tr_mu);
        return;
    }
    __atomic_store_n(slot, (uint64_t)(uintptr_t)b->code, __ATOMIC_RELEASE);
    /* And the stub's B, when the target is in its range: a direct branch, no load. */
    uint32_t *site = m->slot_site[slot - m->slots];
    int64_t off = site ? b->code - site : 0;
    if (site && off >= -(1 << 25) && off < (1 << 25)) {
        uint32_t insn = 0x14000000u | (uint32_t)(off & 0x3ffffff);
        hv_jit_write(&m->jit, site, &insn, 4);
        hv_jit_finish_branch(&m->jit, site); /* B to B; the stub follows the slot meanwhile */
    }
    uint32_t *cond = m->slot_cond[idx];
    if (cond) { /* the conditional branch to that stub, too */
        uint32_t insn = *cond;
        if (hv_branch_retarget(&insn, b->code - cond)) {
            hv_jit_write(&m->jit, cond, &insn, 4);
            hv_jit_finish(&m->jit, cond, 1);
        }
    }
    m->stats.chained++;
}

static int run_loop(HvCpu *cpu, int one);

static int run(HvCpu *cpu, int one) {
    int r = run_loop(cpu, one);
    park(cpu);
    hv_self = NULL;
    return r;
}

/* A powered-off CPU waits here, without the big lock, for PSCI CPU_ON. */
static void wait_while_off(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    hv_big_unlock(m);
    park(cpu);
    pthread_mutex_lock(&p->wake_mu);
    __atomic_store_n(&cpu->parked_off, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&cpu->off, __ATOMIC_ACQUIRE) && !cpu->halted) {
        p->wake_pending = 0;
        pthread_cond_wait(&p->wake_cv, &p->wake_mu);
    }
    __atomic_store_n(&cpu->parked_off, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&p->wake_mu);
    unpark(cpu);
    hv_big_lock(m);
}

/* Dropping every translation: the CPU that finds a flush pending waits until
   all the others have parked (at this barrier, asleep or off), flushes, and
   lets them go. Nobody holds a lock while waiting here. */
static void stop_the_world(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    pthread_mutex_lock(&m->stw_mu);
    if (__atomic_load_n(&m->flush_pending, __ATOMIC_SEQ_CST) && !m->stw_active) {
        __atomic_store_n(&m->stw_active, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&m->flush_pending, 0, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&m->stw_mu);
        hv_machine_kick_all(m);
        if (cpu->pc_state) __atomic_store_n(&cpu->pc_state->phase, HV_PHASE_FLUSH, __ATOMIC_RELAXED);
        for (uint32_t spins = 0;; spins++) {
            int busy = 0;
            for (int i = 0; i < m->ncpus; i++)
                if (m->cpus[i] && m->cpus[i] != cpu && __atomic_load_n(&m->cpus[i]->pc_state->active, __ATOMIC_SEQ_CST)) busy = 1;
            if (!busy) break;
            hv_cpu_process_requests(cpu); /* one of them may be waiting on a TLBI of ours */
            if (spins > 100) sched_yield();
        }
        if (m->gen_checks) for (int i = 0; i < m->ncpus; i++) if (m->cpus[i] && m->cpus[i] != cpu && m->cpus[i]->pc_state->active) fprintf(stderr, "hengevm: flush while cpu %d is active\n", i);
        pthread_mutex_lock(&m->tr_mu);
        hv_flush_blocks(m);
        pthread_mutex_unlock(&m->tr_mu);
        if (cpu->pc_state) __atomic_store_n(&cpu->pc_state->phase, HV_PHASE_RUN, __ATOMIC_RELAXED);
        pthread_mutex_lock(&m->stw_mu);
        __atomic_store_n(&m->stw_active, 0, __ATOMIC_SEQ_CST);
        pthread_cond_broadcast(&m->stw_cv);
        pthread_mutex_unlock(&m->stw_mu);
    } else if (m->stw_active) {
        park(cpu);
        while (m->stw_active) pthread_cond_wait(&m->stw_cv, &m->stw_mu);
        pthread_mutex_unlock(&m->stw_mu);
        unpark(cpu);
    } else {
        pthread_mutex_unlock(&m->stw_mu);
    }
}

/* The kernel's block-by-block path, kept while diagnosing (see run()). */
#define FW_REC 1024
static struct { uint64_t pc, x20, x22, x23, x30, x0; } fw_rec[FW_REC];
static uint32_t fw_n;

#define KTRACE_MAX 8192
static uint64_t ktrace[KTRACE_MAX];
static uint32_t ktrace_n;

int hv_run(HvCpu *cpu) {
    if (cpu->pc_state) cpu->pc_state->thread = mach_thread_self();
    hv_big_lock(cpu->m);
    hv_fp_enter(cpu);
    int r = run(cpu, 0);
    hv_fp_leave(cpu);
    hv_big_unlock(cpu->m);
    return r;
}

int hv_run_one(HvCpu *cpu) {
    hv_big_lock(cpu->m);
    hv_fp_enter(cpu);
    int r = run(cpu, 1);
    hv_fp_leave(cpu);
    hv_big_unlock(cpu->m);
    return r;
}

/* Bring-up aid (log >= 4): report the first block entered with a host address
   in a guest register, which means the translator leaked one. */
static int host_pointer_check(HvCpu *cpu, uint64_t prev_pc) {
    HvMachine *m = cpu->m;
    for (int r = 0; r < 31; r++) {
        uint64_t v = cpu->x[r];
        const char *what = NULL;
        for (int i = 0; i < m->nram && !what; i++)
            if (v - (uint64_t)(uintptr_t)m->ram[i].host < m->ram[i].size && m->ram[i].pa != (uint64_t)(uintptr_t)m->ram[i].host)
                what = "host copy of guest RAM";
        if (v - (uint64_t)(uintptr_t)m->jit.base < m->jit.capacity * 4) what = "JIT region";
        if (v - (uint64_t)(uintptr_t)cpu < sizeof *cpu) what = "HvCpu";
        if (what) {
            fprintf(stderr, "host pointer: x%d = %llx (%s) entering %llx, previous block %llx\n", r,
                    (unsigned long long)v, what, (unsigned long long)cpu->pc, (unsigned long long)prev_pc);
            return 1;
        }
    }
    return 0;
}

static int run_loop(HvCpu *cpu, int one) {
    HvMachine *m = cpu->m;
    uint32_t since_tick = 0;
    uint64_t next_due = 0; /* host time of this CPU's next timer event, from its last tick (0: none armed) */
    uint64_t last_fault = ~0ull, prev_pc = 0;
    HvBlock *prev_b = NULL;
    uint64_t *pending_chain = NULL;
    uint64_t chain_pc = 0;    /* where that exit was going, and in which state */
    uint32_t chain_flags = 0;
    uint32_t last_reason = HV_EXIT_BRANCH;
    hv_self = cpu;
    /* Always: a block the host refuses to run is a bug in the translator, and the only record of
       which guest instruction produced it is written from this handler. It costs one sigaction. */
    jit_fault_watch(m);
    {
        static int watch_set;
        if (!watch_set) {
            watch_set = 1;
            const char *wv = getenv("HV_WATCH_VA");
            if (wv && *wv) m->watch_va = strtoull(wv, NULL, 16);
            const char *wp = getenv("HV_WATCH_PA");
            if (wp && *wp) m->watch_pa = strtoull(wp, NULL, 16);
        }
    }
    unpark(cpu);
    for (;;) {
        if (__atomic_load_n(&m->stop, __ATOMIC_RELAXED)) return HV_EXIT_HALT;
        if (__atomic_load_n(&m->flush_pending, __ATOMIC_SEQ_CST) || __atomic_load_n(&m->stw_active, __ATOMIC_SEQ_CST)) {
            stop_the_world(cpu);
            pending_chain = NULL;
            prev_b = NULL;
            continue;
        }
        hv_cpu_process_requests(cpu);
        if (cpu->halted) return HV_EXIT_HALT;
        if (cpu->off) {
            wait_while_off(cpu); /* comes back after CPU_ON, reset to its entry point */
            pending_chain = NULL;
            last_reason = HV_EXIT_EXCEPTION;
            continue;
        }
        /* Someone asked the CPU to leave (the 1 ms kicker, a device, an invalidation): service devices now. */
        int kicked = __atomic_exchange_n(&cpu->exit_req, 0, __ATOMIC_RELAXED) != 0;
        if (kicked && m->big_lock_on) {
            /* Give another CPU its turn. Whatever it does in between may touch
               what this CPU's exclusive monitor watches, so the monitor clears. */
            cpu->excl_addr = ~0ull;
            hv_big_yield(m);
        }
        /* Ticks take the device lock, so not every few blocks: on a kick (the 1 ms
           kicker, a device with work for CPU 0, an interrupt raised for this
           CPU) or once this CPU's own timer is due. */
        if (m->tick && (kicked || (++since_tick >= 64 && (since_tick = 0, next_due && mach_absolute_time() >= next_due)))) {
            since_tick = 0;
            next_due = m->tick(m, cpu);
            cpu->timer_due = next_due;
        }
        int why = pending_chain ? 0 : kicked ? 1 : last_reason != HV_EXIT_BRANCH ? 2 : 5;
        if (hv_check_interrupts(cpu)) {
            pending_chain = NULL;
            why = 3;
        }
        if (cpu->pc == m->stop_pc || cpu->pc == m->stop_pc2) {
            if (!m->stop_hook) return HV_EXIT_HALT;
            m->stop_hook(cpu);
            pending_chain = NULL; /* every call must come back through here */
        }

        if ((m->log || m->gen_checks) && cpu->el && (cpu->sctlr_el1 & 1) && !((cpu->pc >> 55) & 1) && cpu->pc >= 0x100000000ull) { /* EL1 at a user address: how did we get here? */
            static int reported;
            if (reported++ < 4) {
                fprintf(stderr, "hengevm: el1 at user pc %llx: previous block %llx flags %x (%u insns), exit reason %u, why %d, "
                                "miss site %llx, elr %llx spsr %llx x30 %llx, fast resumes %llu\n  recent:",
                        (unsigned long long)cpu->pc, (unsigned long long)prev_pc, prev_b ? prev_b->flags : 0, prev_b ? prev_b->ninsns : 0,
                        last_reason, why, (unsigned long long)cpu->miss_site, (unsigned long long)cpu->elr_el1,
                        (unsigned long long)cpu->spsr_el1, (unsigned long long)cpu->x[30], (unsigned long long)m->stats.fast_resumes);
                for (int k = 0; k < 16; k++)
                    fprintf(stderr, " %llx", (unsigned long long)m->stats.recent[(m->stats.nrecent + k) & 15]);
                fprintf(stderr, "\n");
            }
        }
        if (m->log && !cpu->el && ((cpu->pc >> 55) & 1)) { /* EL0 can never run kernel code: say how we got here */
            static int reported;
            if (reported++ < 4)
                fprintf(stderr, "hengevm: el0 at kernel pc %llx: previous block %llx, exit reason %u, why %d, miss site %llx, "
                                "elr %llx spsr %llx x30 %llx\n",
                        (unsigned long long)cpu->pc, (unsigned long long)prev_pc, last_reason, why,
                        (unsigned long long)cpu->miss_site, (unsigned long long)cpu->elr_el1,
                        (unsigned long long)cpu->spsr_el1, (unsigned long long)cpu->x[30]);
        }
        uint64_t pa;
        uint32_t avail;
        const uint32_t *code = hv_mmu_fetch(cpu, cpu->pc, &pa, &avail);
        if (!code) { /* the instruction abort was taken */
            /* The pc is the vector now, not the branch target: the exit that got
               here must not be chained to whatever runs next (that put EL0 code
               on a path straight into the kernel's vector code). */
            pending_chain = NULL;
            if (cpu->pc == last_fault) return HV_EXIT_FAULT_LOOP;
            last_fault = cpu->pc;
            continue;
        }
        last_fault = ~0ull;
        uint64_t flushes = m->stats.block_flushes;
        HvBlock *b = lookup(cpu, cpu->pc, pa, block_flags(cpu), code, avail);
        /* A full flush while translating freed the slot the last exit came through,
           and new blocks may have it now: chaining it would rewrite one of theirs. */
        if (m->stats.block_flushes != flushes) pending_chain = NULL;
        if (!b) {
            /* Everything is about to be dropped, or just was (another CPU may have
               done the flush this lookup asked for): try again after. */
            if (m->flush_pending || __atomic_load_n(&m->stw_active, __ATOMIC_SEQ_CST) || m->stats.block_flushes != flushes) continue;
            fprintf(stderr, "hengevm: cannot translate block at %llx\n", (unsigned long long)cpu->pc);
            park(cpu);
            return HV_EXIT_UNSUPPORTED;
        }
        /* Remember it for indirect branches (not the tests' stop PC: that must reach us). */
        /* EL0 tables only hold user VAs and EL1 tables kernel VAs, so each half has its own salt. */
        int kernel = b->flags & HV_BLOCK_EL1;
        if (m->chain && b->va != m->stop_pc && b->va != m->stop_pc2 && (int)((b->va >> 55) & 1) == kernel) {
            HvPerCpu *pcs = cpu->pc_state;
            uint64_t *t = pcs->ijc + (size_t)(b->flags & (HV_IJC_TABLES - 1)) * (HV_IJC_TABLE_BYTES / 8);
            uint64_t tag = b->va ^ cpu->ijc_salt[kernel];
            uint64_t *ent = t + (size_t)hv_ijc_index(tag) * 2;
            if (why == 5 && (ent[0] == tag || ent[2] == tag)) {
                why = 4; /* it was cached: not a miss */
                static int dbg = -1, shown;
                if (dbg < 0) dbg = getenv("HV_IJC_DEBUG") != NULL;
                if (dbg) { /* what instruction came just before the target (put_exit leaves with pc + 4) */
                    static uint64_t hist[4], total;
                    uint32_t before = 0;
                    uint64_t off = b->pa - b->ram->pa;
                    if (off & 0x3fff) memcpy(&before, b->ram->host + off - 4, 4);
                    int k = cpu->miss_site ? 0 : before == 0xd5033fdf ? 1 : (before & 0xfff00000u) == 0xd5100000u ? 2 : 3;
                    hist[k]++;
                    if (++total % 200000 == 0)
                        fprintf(stderr, "ijc cached-miss kinds: ijc site %llu, after ISB %llu, after MSR %llu, other %llu\n",
                                (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2],
                                (unsigned long long)hist[3]);
                    static uint64_t msrs;
                    if (k == 2 && ++msrs % 20000 == 0)
                        fprintf(stderr, "ijc cached-miss msr: S%u_%u_C%u_C%u_%u\n", 2 + ((before >> 19) & 1), (before >> 16) & 7,
                                (before >> 12) & 15, (before >> 8) & 15, (before >> 5) & 7);
                    static uint64_t others;
                    if (k == 3 && ++others % 50000 == 0 && shown < 60) {
                        shown++;
                        fprintf(stderr, "ijc cached-miss other: target %llx el%u reason %u, insn before %08x\n", (unsigned long long)b->va, cpu->el, last_reason, before);
                    }
                }
                if (0) {
                    shown++;
                    uint32_t last = 0;
                    if (prev_b && prev_b->ninsns)
                        memcpy(&last, prev_b->ram->host + (prev_b->pa - prev_b->ram->pa) + 4 * (prev_b->ninsns - 1), 4);
                    fprintf(stderr, "ijc cached-miss: target %llx flags %x from site %llx (prev block %llx flags %x, %u insns, last %08x, reason %u)\n",
                            (unsigned long long)b->va, b->flags, (unsigned long long)cpu->miss_site,
                            (unsigned long long)prev_pc, prev_b ? prev_b->flags : 0, prev_b ? prev_b->ninsns : 0, last, last_reason);
                }
            }
            if (m->stats_interval) { /* what the fill replaces: nothing (cold), another salt's entry, or another target */
                uint64_t old = ent[0];
                int kind = !old ? 0 : (old >> 48) != (tag >> 48) ? 1 : 2;
                if (kind == 1) { /* another salt: is it still in use, or dead (dropped address space, reset)? */
                    uint32_t c = (uint32_t)(old >> 48);
                    int alive = kernel ? (c ^ 0xffff) == cpu->kernel_gen : cpu->asid_cid[cpu->cid_asid[c]] == c;
                    if (!alive) kind = 3;
                }
                m->stats.fill_kind[kind]++;
            }
            if (hv_ijc_fill_checked(cpu, b) && m->stats_interval) m->stats.ijc_fills++;
        }
        /* The block before left through a chainable direct branch to this one: link them. */
        /* Only if nothing in between (an abort, an interrupt) sent the CPU elsewhere. */
        if (pending_chain && !m->trace && b->va == chain_pc && b->flags == chain_flags) chain(m, pending_chain, b);
        pending_chain = NULL;
        if (m->trace) fprintf(stderr, "block %llx el%u\n", (unsigned long long)cpu->pc, cpu->el);
        /* The last blocks a CPU entered, for a crash report to show what led to it: a fault says
           where the guest died, not how it got there. Kept only while diagnosing (log >= 2), and
           it is one store per block, which is why it can be on at all. */
        if (m->log >= 2) hv_pc_hist_note(cpu);
        /* The kernel's own execution order, from its entry to wherever it stops: with no symbols
           and no console, the sequence of blocks is the only account of what the initialisation
           actually did, and where it departed from what it should have done. */
        static int boot_args_fixed;
        if (m->log >= 2 && cpu->pc < 0xfffffe0000000000ull) {
            /* What the firmware had in the registers that decide the kernel entry, just before it
               jumps: x20 holds the link-time entry, x22/x23 the relocation delta, x30 the target. */
            fw_rec[fw_n % FW_REC].pc = cpu->pc;
            fw_rec[fw_n % FW_REC].x20 = cpu->x[20];
            fw_rec[fw_n % FW_REC].x22 = cpu->x[22];
            fw_rec[fw_n % FW_REC].x23 = cpu->x[23];
            fw_rec[fw_n % FW_REC].x30 = cpu->x[30];
            fw_rec[fw_n % FW_REC].x0 = cpu->x[0];
            fw_n++;
        }
        /* The rebase of the kernel image is not a diagnostic: with it skipped the guest runs on
           raw chained-fixup words and dies early, so it must not be tied to the log level. The
           ktrace snapshots below stay behind log 2 — only the fixup runs in every configuration. */
        if (cpu->pc >= 0xfffffe0000000000ull && (m->log >= 2 || !m->kimage_pa) && ktrace_n < KTRACE_MAX) {
            /* Did the firmware patch its own code before jumping? iBEC picks the kernel entry
               with a constant in an adrp/add pair at 0x748980c4; if that constant was rewritten
               at run time, the page holding it differs from what it held when first executed.
               Snapshot the page both times and compare offline. */
            /* To a file, not to stderr: at the moment the kernel starts, the stderr pipe loses
               output (every probe that printed here went missing while the file writes worked). */
            if (ktrace_n == 0) {
                /* The loader's half of the hand-off: find the kernel collection in RAM and rebase
                   its chained fixups, which iBoot would have done before jumping. */
                {
                    static int fx_done;
                    if (!fx_done) {
                        fx_done = 1;
                        uint64_t off_k = 0;
                        uint32_t last_i = (fw_n + FW_REC - 1) % FW_REC;
                        if (fw_n) off_k = fw_rec[last_i].x22 - fw_rec[last_i].x23; /* PA -> VA */
                        uint8_t h[4096];
                        for (uint64_t pa = 0x70000000ull; pa < 0xf0000000ull; pa += 0x1000) {
                            if (hv_phys_read(cpu->m, pa, h, sizeof h) != 0) continue;
                            if (!kf_is_kernel(h)) continue;
                            cpu->m->kimage_pa = pa, cpu->m->kimage_va = pa + off_k;
                            if (getenv("HV_SLOW_MEM")) cpu->m->slow_mem = 1;
                            /* HV_NO_KFIX=1 skips the rebase. iBoot is supposed to have done it
                               already, and doing it twice adds the base twice; which of the two
                               is true for a given firmware is exactly what this switch is for. */
                            /* Not ours to do: arm_init() calls kernel_collection_slide(), which
                               rebases every segment of the collection from the same chains. A pass
                               here makes the kernel's slide land on an already-rebased pointer
                               (0x1d1645a0 -> 0x38c7c5a0) and the guest branches into nothing.
                               HV_KFIX=1 runs it anyway, for comparison. */
                            if (getenv("HV_KFIX")) kernel_fixups(cpu->m, pa, pa + off_k);
                            /* The moment worth keeping: the firmware has finished with the image
                               and the kernel has not run yet, so whatever is wrong in it is the
                               firmware's doing alone. */
                            if (getenv("HV_DUMP_DC")) {
                                const char *wd5 = getenv("HV_BLOCK_DUMP");
                                uint8_t *dc = malloc(0x680000);
                                if (wd5 && dc && hv_phys_read(cpu->m, pa + 0x6f8000ull, dc, 0x680000) == 0) {
                                    char dp[512];
                                    snprintf(dp, sizeof dp, "%s", wd5);
                                    char *sl5 = strrchr(dp, '/');
                                    if (sl5) {
                                        snprintf(sl5 + 1, sizeof dp - (size_t)(sl5 + 1 - dp), "dc-boot.bin");
                                        FILE *df = fopen(dp, "wb");
                                        if (df) { fwrite(dc, 1, 0x680000, df); fclose(df); }
                                    }
                                }
                                free(dc);
                            }
                            break;
                        }
                    }
                }
                const char *wd = getenv("HV_BLOCK_DUMP");
                if (wd && *wd) {
                    char ep[512];
                    snprintf(ep, sizeof ep, "%s", wd);
                    char *sl3 = strrchr(ep, '/');
                    if (sl3) snprintf(sl3 + 1, sizeof ep - (size_t)(sl3 + 1 - ep), "entry.txt");
                    FILE *ef = fopen(ep, "w");
                    if (ef) {
                        for (uint32_t k = 0; k < 1024 && k < fw_n; k++) {
                            uint32_t i2 = (fw_n - 1 - k) % FW_REC;
                            fprintf(ef, "fw -%u pc %llx x20 %llx x22 %llx x23 %llx x30 %llx x0 %llx\n",
                                    k, (unsigned long long) fw_rec[i2].pc,
                                    (unsigned long long) fw_rec[i2].x20, (unsigned long long) fw_rec[i2].x22,
                                    (unsigned long long) fw_rec[i2].x23, (unsigned long long) fw_rec[i2].x30,
                                    (unsigned long long) fw_rec[i2].x0);
                        }
                        fprintf(ef, "pc %llx\nprev %llx\nx0 %llx\nx1 %llx\nx2 %llx\nx3 %llx\nel %u\nspsr %llx\n",
                                (unsigned long long) cpu->pc, (unsigned long long) prev_pc,
                                (unsigned long long) cpu->x[0], (unsigned long long) cpu->x[1],
                                (unsigned long long) cpu->x[2], (unsigned long long) cpu->x[3],
                                cpu->el, (unsigned long long) cpu->spsr_el1);
                        fclose(ef);
                    }
                }
            }
            ktrace[ktrace_n++] = cpu->pc;
            /* From the kernel's first block on, every load and store goes through the emulator, so
               the writes of the init sequence can be listed. Blocks already translated for the
               firmware keep their fast paths, which is what keeps the install itself quick. */
            /* iBEC appends nodes to the device tree and leaves boot_args' length at the older,
               smaller value, so the kernel parses a truncated tree. Try the true length. */
            if (!boot_args_fixed) {
                boot_args_fixed = 1;
                for (uint64_t pa = 0; pa < (1ull << 32); pa += 0x4000) {
                    const uint64_t *q = hv_phys_ptr(m, pa, 0x1000);
                    if (!q) continue;
                    const uint16_t *h = (const uint16_t *) q;
                    if (h[0] < 2 || h[0] > 3 || h[1] < 1 || h[1] > 3) continue;
                    if ((q[1] >> 40) != 0xfffffe) continue;
                    uint32_t *len = (uint32_t *) (uintptr_t) (q + 13);   /* 0x68: device tree length */
                    uint32_t was = *len;
                    if (!was || was >= (16u << 20)) continue;            /* not the real one */
                    *len = 16u << 20;
                    fprintf(stderr, "hengevm: boot_args dt length %u -> %u (pa %llx)\n",
                            was, *len, (unsigned long long) pa);
                    break;
                }
                /* The scan above has not found it once in practice; iBEC places boot_args at
                   the same physical address every boot, so fall back to that. */
                {
                    uint64_t *q = hv_phys_ptr(m, 0x837f0000ull, 0x1000);
                    if (q) {
                        uint32_t *len = (uint32_t *) (uintptr_t) (q + 13);
                        uint32_t was = *len;
                        if (was && was < (16u << 20)) {
                            *len = 16u << 20;
                            fprintf(stderr, "hengevm: boot_args dt length %u -> %u (pa 837f0000 fixed)\n",
                                    was, *len);
                            /* The kernel prints nothing without -v, and its own words about what
                               it does next are worth more than anything inferred from registers.
                               The command line follows the device tree fields at 0x6c. */
                            char *cmd = (char *) q + 0x6c;
                            if (!getenv("HV_QUIET_BOOT") && !strstr(cmd, "-v")) {
                                size_t n = strnlen(cmd, 1000);
                                if (n + 3 < 1024) { memcpy(cmd + n, " -v", 4); }
                                fprintf(stderr, "hengevm: boot-args now \"%s\"\n", cmd);
                            }
                        } else {
                            fprintf(stderr, "hengevm: boot_args at 837f0000 has dt length %u; scan found none\n", was);
                        }
                    } else {
                        fprintf(stderr, "hengevm: boot_args: scan found none and 837f0000 is not RAM here\n");
                    }
                }
            }
            if (!m->slow_mem) {
                m->slow_mem = 1;
                __atomic_store_n(&m->flush_pending, 1, __ATOMIC_SEQ_CST);  /* re-translate with it */
            }
        }
        /*
         * A register the block changed without its own instructions changing it: the block's entry
         * value is in the ring, the value it left behind is in the slot. Guest code writes x16 for
         * itself all the time, so the telling case is a host address appearing, which no guest can
         * make — and then the block's own code is not the writer, a call it made is.
         */
        if (m->log >= 2 && hv_looks_host(cpu->x[16])) {
            const uint64_t *e = NULL;
            for (uint32_t k = 1; k <= 32; k++) {   /* chained blocks may have run since */
                const uint64_t *c = &cpu->trace_ring[((cpu->trace_idx - k) & 255) * 4];
                if (c[0] == prev_pc) { e = c; break; }
            }
            if (e && !hv_looks_host(e[2])) {
                static uint64_t last_report;
                if (last_report != prev_pc) {
                    last_report = prev_pc;
                    fprintf(stderr, "hengevm: block %llx left x16 %llx but entered with %llx (%s)\n",
                            (unsigned long long) prev_pc, (unsigned long long) cpu->x[16],
                            (unsigned long long) e[2], host_name(cpu->x[16]));
                }
            }
        }
        /* The first blocks of a boot, with the register that keeps going wrong: whether it starts
           out wrong says whether the guest wrote it or it was handed over that way. */
        if (m->log >= 2 && cpu->icount < 32)
            fprintf(stderr, "hengevm: icount %llu block %llx el%u x16 %llx x4 %llx\n",
                    (unsigned long long) cpu->icount, (unsigned long long) prev_pc, cpu->el,
                    (unsigned long long) cpu->x[16], (unsigned long long) cpu->x[4]);
        if (m->log >= 2 && cpu->icount < 32) {
            /* The ring says what each block was entered with: whether the block about to run got
               a bad x16 or made one itself. */
            for (uint32_t k = 3; k >= 1; k--) {
                const uint64_t *e = &cpu->trace_ring[((cpu->trace_idx - k) & 255) * 4];
                fprintf(stderr, "hengevm:   ring -%u: va %llx x16slot %llx x16host %llx\n", k,
                        (unsigned long long) e[0], (unsigned long long) e[1],
                        (unsigned long long) e[2]);
            }
            if (hv_looks_host(cpu->x[16]))
                fprintf(stderr, "hengevm:   x16 is %s\n", host_name(cpu->x[16]));
        }
        if (m->log >= 4 && host_pointer_check(cpu, prev_pc)) return HV_EXIT_UNSUPPORTED;
        /*
         * A guest register holding one of this process's own addresses, seen for the first time
         * here: the block that just ended left it that way. Nothing in the guest can produce an
         * address in this library, so it came from host code, and the block and the way it left
         * name which code that was.
         */
        if (m->log >= 2) {
            static uint64_t was[8];
            static uint64_t was4[8], was0[8];
            uint32_t ci = cpu->index < 8 ? cpu->index : 0;
            uint64_t v16 = cpu->x[16], v4 = cpu->x[4], v0 = cpu->x[0];
            int h16 = hv_looks_host(v16), h4 = hv_looks_host(v4);
            int was16 = hv_looks_host(was[ci]), was44 = hv_looks_host(was4[ci]);
            if ((h16 && !was16) || (h4 && !was44))
                fprintf(stderr, "hengevm: host address in a guest register: cpu %u after block %llx "
                                "(prev block %llx, why %d) x16 %llx x4 %llx x0 %llx pc %llx el%u\n",
                        (unsigned) cpu->index, (unsigned long long) prev_pc,
                        (unsigned long long) cpu->pc, why, (unsigned long long) v16,
                        (unsigned long long) v4, (unsigned long long) v0,
                        (unsigned long long) cpu->pc, cpu->el);
            was[ci] = v16, was4[ci] = v4, was0[ci] = v0;
        }
        prev_pc = cpu->pc;
        prev_b = b;
        if (m->stats_interval) m->stats.why[why]++;
        m->stats.recent[m->stats.nrecent++ & 15] = cpu->pc | cpu->el;
        if (why == 5 && m->stats_interval) {
            int kind = 6;
            /* A plain walk: fetching could raise an abort in the guest. */
            HvWalk w;
            uint32_t i = 0;
            if (cpu->miss_site) hv_mmu_walk(cpu, cpu->miss_site, HV_ACC_EXEC, cpu->el, &w);
            if (cpu->miss_site && !w.fsc && hv_phys_read(m, w.pa, &i, 4) == 0) {
                if ((i & 0xfffffc1fu) == 0xd65f0000u || (i & 0xfffffbffu) == 0xd65f0bffu) kind = 0;         /* RET, RETAA/AB */
                else if ((i & 0xfe9f0000u) == 0xd61f0000u) kind = 1; /* BR, BLR and their PAC forms (bit 24) */
                else if ((i & 0x7c000000u) == 0x14000000u) kind = 2;                                         /* B, BL */
                else if ((i & 0xff000010u) == 0x54000000u || (i & 0x7e000000u) == 0x34000000u ||
                         (i & 0x7e000000u) == 0x36000000u) kind = 3;                                        /* B.cond, CB(N)Z, TB(N)Z */
                else if ((i & 0xfff8f01fu) == 0xd500401fu) kind = 4;                                         /* MSR (immediate) */
                else kind = 5;
            }
            m->stats.miss_kind[kind]++;
        }
        cpu->miss_site = 0; /* only the next lookup miss sets it again */
        /* Poll the kernel global the halt came from, by physical address: a store through the
           fast path never reaches the emulator, so watching it means reading it back often. */
        /* The one slot the kernel dies on, at the physical address this boot's boot_args imply:
           if it never changes the kernel never wrote it, and the write that was supposed to happen
           did not. */
        if (m->log >= 2 && !(m->stats.blocks & 0xfff)) {
            /* The kernel relocates its own image per boot, so the slot's address is only known
               through the guest's page tables, resolved here rather than at boot time. */
            static uint64_t watch_seen;
            static uint64_t watch_pa;
            static int watch_n, watch_failed;
            if (!watch_pa && cpu->el == 1) {   /* the walk follows the current EL's tables */
                HvWalk w0;
                hv_mmu_walk(cpu, 0xfffffe001f1f88f0ull, HV_ACC_READ, 1, &w0);
                if (w0.fsc) { if (!watch_failed) { watch_failed = 1;
                        fprintf(stderr, "hengevm: slot walk failed (fsc %x) at blocks %llu pc %llx\n",
                                w0.fsc, (unsigned long long) m->stats.blocks, (unsigned long long) cpu->pc); } }
                /* A walk before the MMU is on has no answer: take only a result that lands in
                   guest RAM, and keep retrying until one does. */
                else if (w0.pa >= 0x70000000ull && w0.pa < 0x70000000ull + (8ull << 30)) {
                    watch_pa = w0.pa;
                    fprintf(stderr, "hengevm: slot walk: pa %llx at blocks %llu pc %llx\n",
                            (unsigned long long) w0.pa, (unsigned long long) m->stats.blocks,
                            (unsigned long long) cpu->pc);
                }
            }
            if (watch_pa) {
                uint64_t v = 0;
                int rcw = hv_phys_read(m, watch_pa, &v, 8);
                static int rc_noted;
                if (rcw && !rc_noted) { rc_noted = 1;
                    fprintf(stderr, "hengevm: slot read failed rc %d pa %llx\n", rcw,
                            (unsigned long long) watch_pa); }
                if (rcw == 0 && (v != watch_seen || !watch_n)) {
                    if (watch_n < 24)
                        fprintf(stderr, "hengevm: slot pa %llx = %llx blocks %llu pc %llx el%u\n",
                                (unsigned long long) watch_pa, (unsigned long long) v,
                                (unsigned long long) m->stats.blocks, (unsigned long long) cpu->pc, cpu->el);
                    watch_seen = v, watch_n++;
                }
            }
        }
        if (m->log >= 2 && !(m->stats.blocks & 0xfff)) {
            static uint64_t seen;
            static int noted, first_fail;
            uint64_t v = 0;
            HvWalk w;   /* the kernel's slide differs per boot: resolve the VA every time */
            hv_mmu_walk(cpu, 0xfffffe001f1f88f0ull, HV_ACC_READ, 1, &w);
            if (w.fsc && !noted && cpu->el == 1 && !first_fail) {
                first_fail = 1;
                fprintf(stderr, "hengevm: global not mapped yet at blocks %llu pc %llx (fsc %x)\n",
                        (unsigned long long) m->stats.blocks, (unsigned long long) cpu->pc, w.fsc);
            }
            if (!w.fsc && hv_phys_read(m, w.pa, &v, 8) == 0 && (v != seen || !noted))
                fprintf(stderr, "hengevm: global %llx (was %llx) blocks %llu pc %llx el%u\n",
                        (unsigned long long) v, (unsigned long long) seen,
                        (unsigned long long) m->stats.blocks,
                        (unsigned long long) cpu->pc, cpu->el),
                seen = v, noted = 1;
        }
        /* A heartbeat: a guest that stops making progress (waiting for an interrupt that will not
           come, or spinning) is otherwise silent, and silence looks the same as a hang. */
        if (m->log >= 2) {
            static uint64_t last_beat, last_idle_pc;
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now = (uint64_t) ts.tv_sec;
            if (now != last_beat) {
                last_beat = now;
                if (kf_dc_pa) {
                    uint64_t dc[2] = {0, 0};
                    hv_phys_read(cpu->m, kf_dc_pa, dc, sizeof dc);
                    fprintf(stderr, "hengevm: dc0 %llx %llx\n", (unsigned long long) dc[0],
                            (unsigned long long) dc[1]);
                }
                /* Stalled means the counter of retired instructions did not move at all since
                   the last beat: the CPU is waiting for something, and what it is waiting for is
                   decided by these fields. Printed once per stall, not once per second. */
                {
                    static uint64_t last_icount;
                    static int stalled;
                    if (cpu->icount == last_icount) {
                        if (!stalled) {
                            stalled = 1;
                            fprintf(stderr, "hengevm: STALLED 1s at pc %llx el%u daif %x sp %llx "
                                            "why %d halted %d off %d irq_line %d fiq_line %d "
                                            "cntv_ctl %llx cntv_cval %llx cntp_ctl %llx cntp_cval %llx "
                                            "now %llx x0 %llx x1 %llx x2 %llx x3 %llx\n",
                                    (unsigned long long) cpu->pc, cpu->el, cpu->daif,
                                    (unsigned long long) cpu->sp, cpu->exit_reason, cpu->halted,
                                    cpu->off, cpu->pc_state->irq_line, cpu->pc_state->fiq_line,
                                    (unsigned long long) cpu->cntv_ctl, (unsigned long long) cpu->cntv_cval,
                                    (unsigned long long) cpu->cntp_ctl, (unsigned long long) cpu->cntp_cval,
                                    (unsigned long long) mach_absolute_time(),
                                    (unsigned long long) cpu->x[0], (unsigned long long) cpu->x[1],
                                    (unsigned long long) cpu->x[2], (unsigned long long) cpu->x[3]);
                            hv_pc_hist_dump(cpu);
                        }
                    } else {
                        stalled = 0;
                        last_icount = cpu->icount;
                    }
                }
                hv_mmio_hist_dump(cpu->m);
                fprintf(stderr, "hengevm: beat pc %llx el%u icount %llu blocks %llu why %d "
                                "slow_mem %d memops %llu\n",
                        (unsigned long long) cpu->pc, cpu->el,
                        (unsigned long long) cpu->icount, (unsigned long long) m->stats.blocks, why,
                        m->slow_mem, (unsigned long long) m->stat_slow_mem);
                /* The kernel global the halt came from, sampled once as soon as the kernel runs:
                   a bad value already there came in with the boot data, not from a later store. */
                if (cpu->el == 1 && cpu->pc >= 0xfffffe0000000000ull) {
                    static int sampled;
                    if (!sampled) {
                        sampled = 1;
                        HvWalk w;
                        hv_mmu_walk(cpu, 0xfffffe001f1f88f0ull, HV_ACC_READ, 1, &w);
                        uint64_t v = 0;
                        if (!w.fsc && hv_phys_read(m, w.pa, &v, 8) == 0)
                            fprintf(stderr, "hengevm: sample: [%llx] = %llx (pa %llx) at pc %llx\n",
                                    (unsigned long long) 0xfffffe001f1f88f0ull,
                                    (unsigned long long) v, (unsigned long long) w.pa,
                                    (unsigned long long) cpu->pc);
                        else
                            fprintf(stderr, "hengevm: sample: that global is not readable (fsc %x)\n", w.fsc);
                    }
                }
                /* Stuck in the kernel's halt area: what it arrived with says why, and it says it
                   nowhere else (nothing reaches the serial port). */
                if (cpu->pc >= 0xfffffe001c894000ull && cpu->pc < 0xfffffe001c895000ull) {
                    static int once;
                    if (!once) {
                        once = 1;
                        /* The kernel's own words, when they are not on the serial port: whatever
                           it printed is in RAM. Search by content, so no address from an earlier
                           run is needed; "chosen" is the control, a device tree string that is
                           certainly there, and says whether the search works at all. */
                        static const char *const pats[] = { "chosen", "Darwin Kernel", "panic", "xnu" };
                        for (unsigned k = 0; k < sizeof pats / sizeof pats[0]; k++) {
                            size_t plen = strlen(pats[k]);
                            int found = 0;
                            for (uint64_t pa = 0; pa < (1ull << 32) && !found; pa += (1u << 20)) {
                                const char *base = hv_phys_ptr(m, pa, 1u << 20);
                                if (!base) continue;
                                for (uint32_t off = 0; off + plen < (1u << 20); off += 4) {
                                    if (memcmp(base + off, pats[k], plen)) continue;
                                    char text[241];
                                    memcpy(text, base + off, 240);
                                    text[240] = 0;
                                    for (int i = 0; i < 240; i++)
                                        if ((unsigned char) text[i] < 0x20 && text[i] != '\n') text[i] = ' ';
                                    fprintf(stderr, "hengevm: found \"%s\" at pa %llx: %.120s\n",
                                            pats[k], (unsigned long long) (pa + off), text);
                                    found = 1;
                                    break;
                                }
                            }
                            if (!found) fprintf(stderr, "hengevm: \"%s\" not in RAM\n", pats[k]);
                        }
                        /* The recorded path, next to the kernel dump. */
                        {
                            static int kt_done;
                            if (!kt_done) {
                                kt_done = 1;
                                const char *wp = getenv("HV_BLOCK_DUMP");
                                char kp[512];
                                if (wp && *wp) {
                                    snprintf(kp, sizeof kp, "%s", wp);
                                    char *sl2 = strrchr(kp, '/');
                                    if (sl2) snprintf(sl2 + 1, sizeof kp - (size_t)(sl2 + 1 - kp), "ktrace.bin");
                                    FILE *kf = fopen(kp, "wb");
                                    if (kf) {
                                        fprintf(stderr, "hengevm: ktrace %u blocks -> %s\n", ktrace_n, kp);
                                        fwrite(ktrace, 8, ktrace_n, kf);
                                        fclose(kf);
                                    }
                                }
                            }
                        }
                        /* The kernel image itself, dug out of RAM by its Mach-O magic: reading the
                           code around the halt address offline says which function stopped it, and
                           that is not answerable from the registers alone. */
                        {
                            static int dumped;
                            if (!dumped) {
                                dumped = 1;
                                const char *where = getenv("HV_BLOCK_DUMP");
                                char path[512];
                                if (where && *where) {
                                    snprintf(path, sizeof path, "%s", where);
                                    char *slash = strrchr(path, '/');
                                    if (slash) snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "kernel.bin");
                                    for (uint64_t pa = 0; pa < (1ull << 32); pa += (1u << 20)) {
                                        const uint32_t *base = hv_phys_ptr(m, pa, 1u << 20);
                                        if (!base) continue;
                                        uint32_t hit = 0;
                                        for (uint32_t off = 0; off + 4 < (1u << 20); off += 4)
                                            if (base[off / 4] == 0xfeedfacfu || base[off / 4] == 0xcffaedfeu) { hit = off; break; }
                                        if (!hit) continue;
                                        FILE *f = fopen(path, "wb");
                                        if (f) {
                                            /* Page reads rather than one contiguous mapping: the
                                               image spans RAM that is not mapped in one piece, and
                                               the previous version stopped at the first hole. */
                                            uint64_t start = pa + hit;
                                            static uint8_t chunk[65536];
                                            for (uint64_t off = 0; off < (80ull << 20); off += sizeof chunk) {
                                                if (hv_phys_read(m, start + off, chunk, sizeof chunk) != 0)
                                                    memset(chunk, 0, sizeof chunk);
                                                fwrite(chunk, 1, sizeof chunk, f);
                                            }
                                            fclose(f);
                                            fprintf(stderr, "hengevm: kernel dump: pa %llx -> %s (80 MB)\n",
                                                    (unsigned long long) start, path);
                                        }
                                        pa = 1ull << 40;
                                        break;
                                    }
                                }
                            }
                        }
                        /* The boot_args iBoot built for the kernel: physBase, memSize, boot flags,
                           the device tree pointer and the command line all live here, and every
                           one of them is something the kernel's early init trusts. */
                        {
                            static int ba_done;
                            if (!ba_done) {
                                ba_done = 1;
                                for (uint64_t pa = 0; pa < (1ull << 32); pa += 0x4000) {
                                    const uint64_t *q = hv_phys_ptr(m, pa, 0x1000);
                                    if (!q) continue;
                                    const uint16_t *h = (const uint16_t *) q;
                                    if (h[0] < 2 || h[0] > 3 || h[1] < 1 || h[1] > 3) continue;
                                    if ((q[1] >> 40) != 0xfffffe) continue;
                                    static int cands;
                                    if (cands >= 3) break;
                                    if (cands == 0) fprintf(stderr, "hengevm: boot_args candidates:\n");
                                    cands++;
                                    fprintf(stderr, "hengevm: boot_args at pa %llx (physBase %llx memSize %llx):\n",
                                            (unsigned long long) pa, (unsigned long long) q[2],
                                            (unsigned long long) q[3]);
                                    /* 0..13, then past the 1024-byte CommandLine, where the flags and
                                       the device tree pointer are. */
                                    for (int i = 0; i < 14; i++)
                                        fprintf(stderr, "hengevm:   [%02d] %016llx%s", i,
                                                (unsigned long long) q[i], (i % 3 == 2) ? "\n" : "  ");
                                    fprintf(stderr, "\n");
                                    /* q[12] is the device tree, as a virtual address in the kernel's
                                       linear map: physBase/virtBase turn it into a physical one. */
                                    if (q[12] > q[1]) {
                                        uint64_t dtpa = q[12] - q[1] + q[2];
                                        /* /chosen/random-seed is what PE_get_random_seed() hands to
                                           early_random_init(), and iBoot leaves it zeroed: every key
                                           the boot derives then comes from a known constant. Fill it
                                           from the host's own entropy instead -- the property is a
                                           32-byte name field, a u32 length and the value. */
                                        {
                                            size_t cap = 256u << 10;
                                            uint8_t *dt = malloc(cap);
                                            int ok = dt != NULL;
                                            for (size_t k = 0; ok && k < cap; k += (64u << 10))   /* phys_read is chunk-sized */
                                                ok = hv_phys_read(m, dtpa + k, dt + k, 64u << 10) == 0;
                                            if (ok) {
                                                uint32_t filled = 0;
                                                for (size_t k = 0; k + 36 <= cap; k++) {
                                                    if (memcmp(dt + k, "random-seed", 12)) continue;
                                                    uint32_t n = 0;
                                                    memcpy(&n, dt + k + 32, 4);
                                                    if (n < 64 || n > 256 || k + 36 + n > cap) continue;
                                                    int zero = 1;
                                                    for (uint32_t b = 0; b < n && zero; b++) zero = dt[k + 36 + b] == 0;
                                                    if (!zero) continue;   /* already seeded: not ours to change */
                                                    uint8_t seed[64];
                                                    int got = derive_seed(seed, sizeof seed);
                                                    if (!got) continue;
                                                    for (uint32_t b = 0; b < n; b++) dt[k + 36 + b] = seed[b % sizeof seed];
                                                    if (hv_phys_write(m, dtpa + k + 36, dt + k + 36, n) == 0) filled = n;
                                                }
                                                fprintf(stderr, "hengevm: random-seed: %u bytes from the %s\n",
                                                        filled, "host");
                                            }
                                            free(dt);
                                        }
                                        const char *where2 = getenv("HV_BLOCK_DUMP");
                                        char p2[512];
                                        if (where2 && *where2) {
                                            snprintf(p2, sizeof p2, "%s", where2);
                                            char *sl = strrchr(p2, '/');
                                            if (sl) snprintf(sl + 1, sizeof p2 - (size_t)(sl + 1 - p2), "dt.bin");
                                            FILE *f2 = fopen(p2, "wb");
                                            if (f2) {
                                                for (uint64_t k = 0; k < (256u << 10); k += (1u << 16)) {
                                                    const void *chunk = hv_phys_ptr(m, dtpa + k, 1u << 16);
                                                    if (!chunk) break;
                                                    fwrite(chunk, 1, 1u << 16, f2);
                                                }
                                                fclose(f2);
                                                fprintf(stderr, "hengevm: dt at va %llx pa %llx -> %s\n",
                                                        (unsigned long long) q[12],
                                                        (unsigned long long) dtpa, p2);
                                            }
                                        }
                                    }
                                    for (int i = 138; i < 170; i++)
                                        fprintf(stderr, "hengevm:   [%03d] %016llx%s", i,
                                                (unsigned long long) q[i], (i % 3 == 2) ? "\n" : "  ");
                                    fprintf(stderr, "\n");
                                }
                            }
                        }
                        /* The device tree the firmware built: its `chosen` node is what decides the
                           kernel's memory layout and boot arguments, so it is the first thing to
                           compare against what the kernel expects. Found by the FDT magic. */
                        {
                            static int dt_done;
                            if (!dt_done) {
                                dt_done = 1;
                                const char *where = getenv("HV_BLOCK_DUMP");
                                char path[512];
                                if (where && *where) {
                                    snprintf(path, sizeof path, "%s", where);
                                    char *slash = strrchr(path, '/');
                                    if (slash) snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "dt2.bin");
                                    for (uint64_t pa = 0; pa < (1ull << 32); pa += (1u << 20)) {
                                        const uint32_t *base = hv_phys_ptr(m, pa, 1u << 20);
                                        if (!base) continue;
                                        /* The magic may be absent (a rebuilt tree), so fall back to
                                           where the tree's own strings are: "chosen" and "dram-size". */
                                        uint32_t hit = 0;
                                        for (uint32_t off = 0; off + 4 < (1u << 20); off += 4)
                                            if (base[off / 4] == 0xedfe0dd0u) { hit = off; break; }
                                        if (!hit) {
                                            const char *mb = (const char *) base;
                                            for (uint32_t off = 0; off + 8 < (1u << 20); off++)
                                                if (!memcmp(mb + off, "chosen", 6)) { hit = off & ~3u; break; }
                                        }
                                        if (!hit) continue;
                                        FILE *f = fopen(path, "wb");
                                        if (f) {
                                            fwrite((const char *) base + hit, 1, (1u << 20) - hit, f);
                                            const void *more = hv_phys_ptr(m, pa + (1u << 20), 1u << 20);
                                            if (more) fwrite(more, 1, 1u << 20, f);
                                            fclose(f);
                                            fprintf(stderr, "hengevm: device tree dump: pa %llx -> %s\n",
                                                    (unsigned long long) (pa + hit), path);
                                        }
                                        pa = 1ull << 40;
                                        break;
                                    }
                                }
                            }
                        }
                        /* A kernel address in a register at the halt is likely the text of the
                           reason: read it back through the guest's own page tables. */
                        for (int a = 0; a < 4; a++) {
                            uint64_t va = cpu->x[a];
                            if (va < 0xfffffe0000000000ull) continue;
                            HvWalk w;
                            hv_mmu_walk(cpu, va, HV_ACC_READ, 1, &w);
                            char text[129];
                            int n = 0;
                            if (!w.fsc && hv_phys_read(m, w.pa, text, 128) == 0) {
                                text[128] = 0;
                                for (; n < 128 && text[n]; n++)
                                    if (text[n] < 0x20 || text[n] > 0x7e) break;
                                text[n] = 0;
                                fprintf(stderr, "hengevm: halt x%d -> \"%s\"\n", a, text);
                            }
                        }
                        fprintf(stderr, "hengevm: halt area: x0 %llx x1 %llx x2 %llx x3 %llx x4 %llx "
                                        "x5 %llx x6 %llx x7 %llx elr %llx spsr %llx sp %llx daif %x\n",
                                (unsigned long long) cpu->x[0], (unsigned long long) cpu->x[1],
                                (unsigned long long) cpu->x[2], (unsigned long long) cpu->x[3],
                                (unsigned long long) cpu->x[4], (unsigned long long) cpu->x[5],
                                (unsigned long long) cpu->x[6], (unsigned long long) cpu->x[7],
                                (unsigned long long) cpu->elr_el1, (unsigned long long) cpu->spsr_el1,
                                (unsigned long long) cpu->sp, cpu->daif);
                    }
                }
                /* A guest that sits at one pc is waiting for an interrupt: whether it is taking
                   one is then decided by these, and each of them fails in a different way. */
                if (cpu->pc == last_idle_pc) {
                    /* A spin like this is where panic() ends, and the message it formatted sits in
                       the stack this CPU is on. Scan it for printable runs: the console does not
                       print it, so the stack is the only place it is written down. */
                    if (cpu->el == 1 && getenv("HV_STACKSCAN")) {
                        uint8_t page[256];
                        int printed = 0;
                        for (uint64_t at = cpu->sp; at < cpu->sp + 0x4000 && printed < 8; at += sizeof page) {
                            HvWalk w;
                            hv_mmu_walk(cpu, at, HV_ACC_READ, 1, &w);
                            if (w.fsc != 0 || hv_phys_read(cpu->m, w.pa, page, sizeof page) != 0) continue;
                            int run = 0;
                            for (unsigned k = 0; k < sizeof page; k++) {
                                int ok = page[k] >= 0x20 && page[k] < 0x7f;
                                if (ok) { run++; continue; }
                                if (run >= 24) {
                                    fprintf(stderr, "hengevm: stack %llx: %.*s\n",
                                            (unsigned long long) (at + k - (unsigned) run), run,
                                            (char *) page + k - run);
                                    printed++;
                                }
                                run = 0;
                            }
                        }
                    }
                    fprintf(stderr, "hengevm: idle daif %x irq_line %d fiq_line %d cntp_ctl %llx "
                                    "cntp_cval %llx now %llx cntv_ctl %llx cntv_cval %llx pc %llx\n",
                            cpu->daif, cpu->pc_state->irq_line, cpu->pc_state->fiq_line,
                            (unsigned long long) cpu->cntp_ctl, (unsigned long long) cpu->cntp_cval,
                            (unsigned long long) mach_absolute_time(),
                            (unsigned long long) cpu->cntv_ctl, (unsigned long long) cpu->cntv_cval,
                            (unsigned long long) cpu->pc);
                    hv_pc_hist_dump(cpu);   /* how it got there */
                }
                last_idle_pc = cpu->pc;
            }
        }
        cpu->exit_reason = HV_EXIT_BRANCH;
        cpu->last_stub = 0;
        cpu->icount++;
        m->stats.blocks++;

        if (m->stats_interval && cpu->index == 0 && (m->stats.blocks & 0xffff) == 0) print_stats(cpu, 0);
        if (m->gen_checks && ((uint64_t)(b->code - m->jit.base) >= m->jit.used || b->jit_gen != __atomic_load_n(&m->jit_gen, __ATOMIC_SEQ_CST)))
            fprintf(stderr, "hengevm: cpu %u entering %p (rx+%llx, used %llx) for va %llx: block gen %u, now %u, maker %u\n", cpu->index, (void *)b->code, (unsigned long long)(b->code - m->jit.base) * 4, (unsigned long long)m->jit.used * 4, (unsigned long long)b->va, b->jit_gen, m->jit_gen, b->maker);
        /* Who rebases __DATA_CONST: printed the moment the first word changes, so the value the
           loader wrote and the value the guest later reads are both on record. */
        if (kf_dc_pa && hv_dc_probe && (++dc_poll & 0xf) == 0) {
            uint64_t v = 0;
            hv_phys_read(m, kf_dc_pa, &v, 8);
            if (v != dc_last) {
                fprintf(stderr, "hengevm: dc0 %llx -> %llx at pc %llx icount %llu\n",
                        (unsigned long long) dc_last, (unsigned long long) v,
                        (unsigned long long) cpu->pc, (unsigned long long) cpu->icount);
                dc_last = v;
            }
        }
        hv_enter(cpu, b->code);

        /* An undefined instruction at EL1 is where the panic path traps to the debugger (the
           brk-like word XNU uses): the message panic() formatted is still in the frames above,
           so this is the last moment it can be read. */
        if (m->log >= 2 && cpu->exit_reason == HV_EXIT_EXCEPTION && cpu->el == 1 && !m->trap_dumped &&
            (cpu->esr_el1 >> 26) == 0) {
            m->trap_dumped = 1;
            const char *wd3 = getenv("HV_BLOCK_DUMP");
            if (wd3 && *wd3) {
                /* The path into the trap: the panic frame is not reachable from the SP the vector
                   runs on, but the blocks that ran are, and the call that started this is one of
                   the last few. */
                char pp[512];
                snprintf(pp, sizeof pp, "%s", wd3);
                char *sl9 = strrchr(pp, '/');
                if (sl9) {
                    snprintf(sl9 + 1, sizeof pp - (size_t)(sl9 + 1 - pp), "trap_path.txt");
                    FILE *pf = fopen(pp, "w");
                    if (pf) {
                        hv_pc_hist_dump_path(cpu, pf);
                        fclose(pf);
                    }
                }
            }
            if (wd3 && *wd3) {
                char tp[512];
                snprintf(tp, sizeof tp, "%s", wd3);
                char *sl3 = strrchr(tp, '/');
                if (sl3) {
                    snprintf(sl3 + 1, sizeof tp - (size_t)(sl3 + 1 - tp), "trap.txt");
                    FILE *tf = fopen(tp, "w");
                    if (tf) {
                        {
                            uint64_t sl4[4] = {0, 0, 0, 0};
                            if (cpu->m->kimage_pa) hv_phys_read(cpu->m, cpu->m->kimage_pa + 0x6f8000, sl4, sizeof sl4);
                            fprintf(tf, "dataconst now: %#llx %#llx %#llx %#llx\n",
                                    (unsigned long long) sl4[0], (unsigned long long) sl4[1],
                                    (unsigned long long) sl4[2], (unsigned long long) sl4[3]);
                            /* The watched record: its physical home and what it holds now, which is
                               the value the guest will read next. */
                            if (cpu->m->watch_va) {
                                HvWalk w5;
                                uint64_t v5 = 0;
                                memset(&w5, 0, sizeof w5);
                                hv_mmu_walk(cpu, cpu->m->watch_va, HV_ACC_READ, 1, &w5);
                                if (!w5.fsc) hv_phys_read(cpu->m, w5.pa, &v5, 8);
                                fprintf(tf, "watched %#llx -> pa %#llx value %#llx\n",
                                        (unsigned long long) cpu->m->watch_va, (unsigned long long) w5.pa,
                                        (unsigned long long) v5);
                            }
                        }
                        fprintf(tf, "elr %#llx esr %#x far %#llx sp %#llx\n",
                                (unsigned long long) cpu->elr_el1, cpu->esr_el1,
                                (unsigned long long) cpu->far_el1, (unsigned long long) cpu->sp);
                        for (int i = 0; i < 31; i++) fprintf(tf, "x%d %#llx%s", i, (unsigned long long) cpu->x[i], (i % 4 == 3) ? "\n" : "  ");
                        fprintf(tf, "\n--- stack ---\n");
                        HvWalk sw2;
                        memset(&sw2, 0, sizeof sw2);
                        hv_mmu_walk(cpu, cpu->sp, HV_ACC_READ, 1, &sw2);
                        static uint8_t b3[8192];
                        if (!sw2.fsc) {
                            uint64_t pa0 = sw2.pa;
                            for (uint64_t o = 0; o < 0x8000; o += sizeof b3) {
                                if (hv_phys_read(cpu->m, pa0 + o, b3, sizeof b3) != 0) break;
                                for (size_t z = 0; z < sizeof b3; z++)
                                    fputc((b3[z] >= 0x20 && b3[z] < 0x7f) || b3[z] == '\n' ? b3[z] : '.', tf);
                                fputc('\n', tf);
                            }
                        }
                        fclose(tf);
                    }
                }
            }
        }
        if (m->log >= 2 && cpu->pc == 0xfffffe001c894314ull && !m->halt_dumped) {
            m->halt_dumped = 1;
            hv_pc_hist_dump_file(cpu);
        }
        if (cpu->exit_reason == HV_EXIT_BRANCH && cpu->last_stub && m->chain) {
            pending_chain = (uint64_t *)cpu->last_stub;
            chain_pc = cpu->pc;
            chain_flags = block_flags(cpu); /* the exiting block's state (a block ends where its state changes) */
        }
        last_reason = cpu->exit_reason;
        switch (cpu->exit_reason) {
        case HV_EXIT_BRANCH:
        case HV_EXIT_EXCEPTION:
        case HV_EXIT_SYSREG:
        case HV_EXIT_FLUSH:
            break;
        case HV_EXIT_WFI:
            if (wait_for_interrupt(cpu) != 0) return HV_EXIT_WFI;
            break;
        default:
            return (int)cpu->exit_reason;
        }
        if (one) return HV_EXIT_BRANCH;
    }
}

/* ---- the blocks a CPU entered last ----------------------------------------

   A fault says where the guest died, not how it got there: the pointer that went bad was usually
   loaded several blocks earlier, and the load that produced it is what a report has to name. This
   ring is filled while m->log >= 2 — one store per block entry — and printed when the guest
   reports its own panic. */

#define PC_HIST 4096
static uint64_t pc_hist[HV_MAX_CPUS][PC_HIST];
static uint32_t pc_hist_at[HV_MAX_CPUS];

void hv_pc_hist_note(HvCpu *cpu) {
    if (!cpu) return;
    hv_pc_hist_note_va(cpu->index, cpu->pc);
}

void hv_pc_hist_note_va(int index, uint64_t va) {
    if (index < 0 || index >= HV_MAX_CPUS) return;
    uint32_t i = pc_hist_at[index]++ & (PC_HIST - 1);
    pc_hist[index][i] = va;
}

uint64_t hv_pc_hist_last(int index) {
    if (index < 0 || index >= HV_MAX_CPUS) return 0;
    uint32_t at = pc_hist_at[index];
    if (at == 0) return 0;
    return pc_hist[index][(at - 1) & (PC_HIST - 1)];
}

/* The guest reached the idle spin the restore never comes out of: the ring at this instant still
   holds the path that got there, and a few million blocks later it does not. */
void hv_pc_hist_dump_file(HvCpu *cpu) {
    const char *wd = getenv("HV_BLOCK_DUMP");
    if (!wd || !*wd || !cpu || cpu->index >= HV_MAX_CPUS) return;
    char p[512];
    snprintf(p, sizeof p, "%s", wd);
    char *sl = strrchr(p, '/');
    if (sl) snprintf(sl + 1, sizeof p - (size_t)(sl + 1 - p), "halt.txt");
    FILE *f = fopen(p, "w");
    if (!f) return;
    uint32_t at = pc_hist_at[cpu->index];
    fprintf(f, "halt pc %#llx el%u icount %llu\n", (unsigned long long) cpu->pc, cpu->el,
            (unsigned long long) cpu->m->stats.blocks);
    fprintf(f, "elr %#llx esr %#x far %#llx sp %#llx sp_el0 %#llx spsr %#llx daif %#llx\n",
            (unsigned long long) cpu->elr_el1, cpu->esr_el1, (unsigned long long) cpu->far_el1,
            (unsigned long long) cpu->sp, (unsigned long long) cpu->sp_el0,
            (unsigned long long) cpu->spsr_el1, (unsigned long long) cpu->daif);
    {
        HvWalk w;
        uint64_t far = cpu->far_el1;
        memset(&w, 0, sizeof w);
        hv_walk_dbg = f;
        hv_mmu_walk(cpu, far, HV_ACC_WRITE, 1, &w);
        hv_walk_dbg = NULL;
        fprintf(f, "walk far %#llx: fsc %#x pa %#llx block %#llx r%u w%u x%u attr %#x pstate_extra %#llx (PAN %d)\n",
                (unsigned long long) far, w.fsc, (unsigned long long) w.pa,
                (unsigned long long) w.block_size, w.r, w.w, w.x, w.attr,
                (unsigned long long) cpu->pstate_extra, (cpu->pstate_extra & HV_PSTATE_PAN) ? 1 : 0);
    }
    /* Every exception the guest took recently, oldest first: the halt reports the last one, and
       what walked the stack into its guard page was usually one of the earlier ones. */
    for (uint32_t n = 0; n < HV_EXC_RING; n++) {
        const HvExcRec *r = &hv_exc_ring[(hv_exc_at + n) & (HV_EXC_RING - 1)];
        if (!r->esr && !r->elr) continue;
        fprintf(f, "exc t%u el%u esr %#x elr %#llx far %#llx sp %#llx\n", r->type, r->el, (unsigned) r->esr,
                (unsigned long long) r->elr, (unsigned long long) r->far, (unsigned long long) r->sp);
    }
    fprintf(f, "tpidr_el1 %#llx x18 %#llx x19 %#llx x20 %#llx x21 %#llx\n",
            (unsigned long long) cpu->tpidr_el1, (unsigned long long) cpu->x[18],
            (unsigned long long) cpu->x[19], (unsigned long long) cpu->x[20],
            (unsigned long long) cpu->x[21]);
    for (uint32_t n = 0; n < PC_HIST; n++)
        fprintf(f, "%llx\n", (unsigned long long) pc_hist[cpu->index][(at + n) & (PC_HIST - 1)]);
    fclose(f);
}

void hv_pc_hist_dump_path(HvCpu *cpu, FILE *f) {
    if (!cpu || cpu->index >= HV_MAX_CPUS) return;
    uint32_t at = pc_hist_at[cpu->index];
    for (uint32_t n = 0; n < PC_HIST; n++)
        fprintf(f, "%llx\n", (unsigned long long) pc_hist[cpu->index][(at + n) & (PC_HIST - 1)]);
}

void hv_pc_hist_dump(HvCpu *cpu) {
    if (!cpu || cpu->index >= HV_MAX_CPUS) return;
    uint32_t at = pc_hist_at[cpu->index];
    uint32_t show = PC_HIST > 64 ? 64 : PC_HIST;
    fprintf(stderr, "hengevm: last %d blocks entered on cpu %u, oldest first:\n",
            show, (unsigned) cpu->index);
    for (uint32_t n = PC_HIST - show; n < PC_HIST; n++)
        fprintf(stderr, "  %llx\n", (unsigned long long) pc_hist[cpu->index][(at + n) & (PC_HIST - 1)]);
}

/* The ring the translated code fills: the exact block path, in order, regardless of how each
   block was reached. */
void hv_trace_ring_dump(HvCpu *cpu) {
    if (!cpu) return;
    /* Only the entries where a register changes value: the ring holds a few hundred blocks and
       the interesting fact is which block first saw the guest's x16 turn into something that is
       not a guest address. Printing only the changes keeps the burst short enough for logcat. */
    fprintf(stderr, "hengevm: traced blocks on cpu %u (changes): va x16(slot) x16(host) x4(slot)\n",
            (unsigned) cpu->index);
    uint64_t last = 0, last4 = 0;
    uint32_t printed = 0;
    for (uint32_t n = 0; n < 256 && printed < 40; n++) {
        const uint64_t *e = &cpu->trace_ring[((cpu->trace_idx + n) & 255) * 4];
        if (e[0] == 0) continue;
        if (n + 8 < 256 && e[1] == last && e[2] == last4) continue;
        last = e[1], last4 = e[2], printed++;
        fprintf(stderr, "  %llx %llx %llx %llx\n", (unsigned long long) e[0],
                (unsigned long long) e[1], (unsigned long long) e[2],
                (unsigned long long) e[3]);
    }
}

/* The last helper calls, oldest first: guest pc, op, and the registers of interest. */
void hv_helper_hist_dump(HvCpu *cpu) {
    if (!cpu) return;
    fprintf(stderr, "hengevm: last helper calls on cpu %u (oldest first): pc op x0 x4 x16 x30\n",
            (unsigned) cpu->index);
    uint32_t at = cpu->helper_hist_idx;
    for (uint32_t n = 48; n < 64; n++) {   /* the last 16 that were used */
        uint32_t i = (at + n) & 63;
        if (cpu->helper_hist[i].pc == 0 && cpu->helper_hist[i].op == 0) continue;
        fprintf(stderr, "  %llx %llu %llx %llx %llx %llx\n",
                (unsigned long long) cpu->helper_hist[i].pc,
                (unsigned long long) cpu->helper_hist[i].op,
                (unsigned long long) cpu->helper_hist[i].x0,
                (unsigned long long) cpu->helper_hist[i].x4,
                (unsigned long long) cpu->helper_hist[i].x16,
                (unsigned long long) cpu->helper_hist[i].x30);
    }
}
