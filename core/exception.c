#include "exception.h"
#include "machine.h"

/* The syscall log: see hv_exception_insn(). */
#define HV_SVC_CAP 4096u
uint64_t hv_svc_pc[HV_SVC_CAP];
uint64_t hv_svc_x16[HV_SVC_CAP], hv_svc_x0[HV_SVC_CAP], hv_svc_x1[HV_SVC_CAP];
uint64_t hv_svc_x2[HV_SVC_CAP], hv_svc_x3[HV_SVC_CAP];
/* The address space a syscall came from: without it, the arguments cannot be read back. */
uint64_t hv_svc_ttbr0[HV_SVC_CAP];
/* The machine a syscall last came from, so the ring can be decoded without a CPU argument. */
static HvMachine *g_svc_m;
uint32_t hv_svc_imm[HV_SVC_CAP], hv_svc_el[HV_SVC_CAP];
uint64_t hv_svc_n;
/* Set when a syscall traps and cleared when userland runs again: a syscall still pending at the
   end is the one the kernel never returned from.  The value written on the way out is x0, which
   is the result the caller got. */
uint64_t hv_svc_ret[HV_SVC_CAP];
/* The path of every open/execve, read out of the guest's own address space at the trap.  Which
   file a process stops after opening is the difference between "it loaded its config and went
   idle" and "it never found what it was looking for". */
char hv_svc_path[HV_SVC_CAP][96];
/* Per CPU: with two CPUs running, a single global "pending" is cleared by whichever CPU returns
   to userland first, which is exactly the information the flag exists to preserve. */
/* Only exceptions taken FROM userland, kept separately: the general ring above holds 128 entries
   and the idle loop fills it with timer interrupts within milliseconds, so a userland fault is
   overwritten long before anyone looks.  These are the ones that end a process. */
#define HV_UEXC_CAP 64u
uint64_t hv_uexc_n;
uint64_t hv_uexc_elr[HV_UEXC_CAP], hv_uexc_far[HV_UEXC_CAP], hv_uexc_pc[HV_UEXC_CAP];
uint32_t hv_uexc_esr[HV_UEXC_CAP], hv_uexc_type[HV_UEXC_CAP];
/* The register file as it stood at an EL0 fault.  The base register of a bad load, or the
   destination of a branch into garbage, exists only in the CPU at that instant: by the time
   anything else looks the process has exited and its address space is gone (walking the page
   tables then fails, which is exactly why the last-block snapshot in dispatch.c also stores a
   physical page).  SVC (EC 0x15) is the normal path and is excluded; 0x20/0x21 are instruction
   aborts from EL0 and 0x24/0x25 data aborts.  Ring of 8: the fault that ends a process is
   usually the last one, but the ones just before say whether it was the first bad pointer or a
   retry loop. */
#define HV_EL0_FLT 8u
uint64_t hv_el0_flt_n;
uint64_t hv_el0_flt_pc[HV_EL0_FLT], hv_el0_flt_ret[HV_EL0_FLT], hv_el0_flt_far[HV_EL0_FLT];
uint64_t hv_el0_flt_sp[HV_EL0_FLT], hv_el0_flt_x[HV_EL0_FLT][31];
uint32_t hv_el0_flt_esr[HV_EL0_FLT];
/* Address space of the faulting thread.  Several user processes interleave, and
   the fault ring is global, so without this a fault cannot be matched to the
   vm_map/pmap that owns it -- hv_last_el0_ttbr0 only names the most recent one. */
uint64_t hv_el0_flt_ttbr0[HV_EL0_FLT];

int hv_svc_pending[HV_MAX_CPUS];
uint64_t hv_svc_last_slot[HV_MAX_CPUS];

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "dispatch.h"
#include "mmu.h"

/* Only EL0 and EL1 exist for the guest; HengeVM plays EL2 (PSCI over HVC, as
   QEMU's vmapple machine sets up) and there is no EL3. */

#define PSTATE_EXTRA_MASK ((1ull << 22) | (1ull << 23) | (1ull << 12) | (1ull << 24) | (1ull << 25))

uint64_t hv_pstate_get(HvCpu *cpu) {
    return (cpu->nzcv & 0xf0000000) | (cpu->daif & HV_PSTATE_DAIF) | (cpu->pstate_extra & PSTATE_EXTRA_MASK) |
           (uint64_t)cpu->el << 2 | (cpu->el ? cpu->spsel : 0);
}

void hv_sp_switch(HvCpu *cpu, uint32_t el, uint32_t spsel) {
    if (cpu->el == 0 || cpu->spsel == 0) cpu->sp_el0 = cpu->sp;
    else cpu->sp_el1 = cpu->sp;
    cpu->el = el;
    cpu->spsel = el ? spsel : 0;
    cpu->sp = cpu->spsel ? cpu->sp_el1 : cpu->sp_el0;
}

static void set_extra(HvCpu *cpu, uint64_t extra) {
    /* PAN selects a different TLB table (hv_tlb_table), so nothing to flush. */
    cpu->pstate_extra = extra;
}

void hv_pstate_set(HvCpu *cpu, uint64_t spsr) {
    uint32_t m = spsr & 0x1f;
    uint32_t el = (m >> 2) & 3;
    if ((m & 0x10) || el > 1) el = 1; /* AArch32 and EL2/EL3 do not exist: an illegal return, kept at EL1 */
    cpu->nzcv = spsr & 0xf0000000;
    cpu->daif = spsr & HV_PSTATE_DAIF;
    set_extra(cpu, spsr & PSTATE_EXTRA_MASK);
    hv_sp_switch(cpu, el, m & 1);
}

/* The last exceptions as the guest saw them, in order: the fault a halt reports is only the last
   one, and the path that led there is usually a different, earlier exception. */
HvExcRec hv_exc_ring[HV_EXC_RING];
uint32_t hv_exc_at;

/* Debug-only fault-path probe.
 *
 * put_trace_entry() writes every block entry into cpu->trace_ring from the translated code
 * itself, so chained blocks and direct bl targets are in it -- unlike HV_TRACE_PC, which only
 * sees blocks that re-enter the dispatcher. That makes it the one instrument that can answer
 * "did this exception's kernel path reach address F".
 *
 * Usage: HV_FAULT_PROBE=lo:hi (run VAs, 32-bit log form accepted; default is pmap_enter_options
 * at slide 0), then read the counters with hv_fault_stats_dump(). Counting happens in hv_eret,
 * i.e. after the handler ran, not at the fault itself. */
#define FPROBE_MAX 4
static uint64_t fprobe_lo[FPROBE_MAX] = {0xfffffe001ca2206cull, 0, 0, 0};
static uint64_t fprobe_hi[FPROBE_MAX] = {0xfffffe001ca233acull, 0, 0, 0};
static int fprobe_n = 1;
static int fprobe_read;
static uint32_t pend_esr[HV_MAX_CPUS];
static uint8_t pend_valid[HV_MAX_CPUS];
static uint64_t pend_far[HV_MAX_CPUS];
static uint64_t pend_ttbr0[HV_MAX_CPUS];
/* Did pmap_enter_options actually leave a mapping behind? Walked at eret, after the handler. */
static uint64_t ptest_n[8], ptest_ok[8], ptest_zero[8], ptest_other[8];
static uint64_t ptest_pn[8], ptest_pok[8], ptest_pzero[8];
/* Is a fault a fresh address (demand paging making progress) or the same page over and over
   (a loop)? A hash of faulting page numbers answers it; collisions only undercount repeats. */
#define FARHASH 8192
static uint64_t far_seen[FARHASH];
static uint64_t far_pg[FARHASH], far_space[FARHASH];
static uint16_t far_cnt[FARHASH];
static uint64_t far_total, far_uniq, far_repeat;
static uint64_t fstat_n[8], fstat_probe[8][FPROBE_MAX];
/* 4 KiB buckets over 0x1ca00000..0x1cc00000 (the region the fault handler and pmap live in). */
#define FHIST_BASE 0xfffffe001ca00000ull
#define FHIST_N (8192u)
static uint32_t fhist[FHIST_N];

static int flt_class(uint32_t esr) {
    uint32_t ec = (esr >> 26) & 0x3f, fsc = esr & 0x3f;
    if (ec == 0x15) return 0;                                  /* svc */
    if (ec == 0x24 || ec == 0x25) {
        if (fsc >= 4 && fsc <= 7) return 1;                    /* translation */
        if (fsc >= 0xd && fsc <= 0xf) return 2;                /* permission */
        if (fsc >= 9 && fsc <= 0xb) return 3;                  /* access flag */
        return 4;
    }
    if (ec == 0x21) return 5;                                  /* instruction abort */
    return 6;
}

static void fprobe_init(void) {
    if (fprobe_read) return;
    fprobe_read = 1;
    const char *e = getenv("HV_FAULT_PROBE");
    if (!e || !*e) return;
    /* Up to four comma separated lo:hi ranges. */
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    fprobe_n = 0;
    for (char *p = buf; p && *p && fprobe_n < FPROBE_MAX; ) {
        char *comma = strchr(p, ',');
        if (comma) *comma = 0;
        uint64_t lo = 0, hi = 0;
        if (sscanf(p, "%llx:%llx", (unsigned long long *) &lo, (unsigned long long *) &hi) == 2) {
            if (hi && hi <= 0xffffffffull) {   /* 32-bit log form */
                lo |= 0xfffffe0000000000ull;
                hi |= 0xfffffe0000000000ull;
            }
            fprobe_lo[fprobe_n] = lo, fprobe_hi[fprobe_n] = hi, fprobe_n++;
        }
        p = comma ? comma + 1 : NULL;
    }
    if (!fprobe_n) { fprobe_lo[0] = 0xfffffe001ca2206cull, fprobe_hi[0] = 0xfffffe001ca233acull, fprobe_n = 1; }
}

static int ring_has(HvCpu *cpu, int which) {
    for (uint32_t n = 0; n < 256; n++) {
        uint64_t va = cpu->trace_ring[((cpu->trace_idx + n) & 255) * 4];
        if (va >= fprobe_lo[which] && va < fprobe_hi[which]) return 1;
    }
    return 0;
}

/* The last syscalls in order, with the arguments and the value the caller got back. A process
   that stops making progress is waiting in one of these: the number says what it is waiting for
   (Darwin takes it in x16) and ret says the call has not returned yet if it is the newest one. */
void hv_svc_dump_tail(void) {
    uint64_t n = hv_svc_n, cnt = n < 48 ? n : 48;
    /* The tail is dominated by the pthread exit sequence (__disable_threadsignal=331,
       bsdthread_terminate=361), which says a thread pool is shrinking, not what the system is
       doing. A mix over the last 4096 calls, plus the last call that is not part of an exit,
       shows the actual work. */
    {
        uint32_t hist[600];
        memset(hist, 0, sizeof hist);
        uint64_t total = n < 4096 ? n : 4096;
        for (uint64_t i = n - total; i < n; i++) {
            uint32_t no = (uint32_t) hv_svc_x16[i & (HV_SVC_CAP - 1)];
            if (no < 600) hist[no]++;
        }
        fprintf(stderr, "hengevm: syscall mix over last %llu:", (unsigned long long) total);
        for (uint32_t no = 0; no < 600; no++)
            if (hist[no]) fprintf(stderr, " %u=%u", no, hist[no]);
        fprintf(stderr, "\n");
        /* mach_msg2_trap (-47) is how libdispatch and IOKit wait for IPC. Its first argument is
           the mach_msg header in guest user memory; msgh_local_port / msgh_id say which service is
           being waited on, which no amount of kernel-structure walking would tell us. */
        {
            HvCpu *cpu = (g_svc_m && g_svc_m->cpus[0]) ? g_svc_m->cpus[0] : NULL;
            int shown = 0;
            for (uint64_t i = n; i-- > 0 && shown < 6 && cpu; ) {
                uint64_t s = i & (HV_SVC_CAP - 1);
                if ((int64_t) hv_svc_x16[s] != -47 || !hv_svc_x0[s]) continue;
                uint64_t saved_ttbr0 = cpu->ttbr0_el1;
                cpu->ttbr0_el1 = hv_svc_ttbr0[s];
                HvWalk w;
                hv_mmu_walk(cpu, hv_svc_x0[s], HV_ACC_READ, 0, &w);
                cpu->ttbr0_el1 = saved_ttbr0;
                if (w.fsc) continue;
                uint8_t buf[0x200];
                if (hv_phys_read(cpu->m, w.pa, buf, sizeof buf) != 0) continue;
                uint32_t bits, size, rport, lport, vport, id;
                memcpy(&bits, buf, 4), memcpy(&size, buf + 4, 4);
                memcpy(&rport, buf + 8, 4), memcpy(&lport, buf + 12, 4);
                memcpy(&vport, buf + 16, 4), memcpy(&id, buf + 20, 4);
                fprintf(stderr, "hengevm: mach_msg2 pc %llx bits %x size %u remote %x local %x"
                                " voucher %x id %d\n",
                        (unsigned long long) hv_svc_pc[s], bits, size, rport, lport, vport,
                        (int) id);
                /* The body after the header: for an IOKit call the selector and arguments live
                   here, and they are what name the service. */
                fprintf(stderr, "hengevm:   body:");
                for (int k = 0x18; k < 0x1b8; k += 8) {
                    uint64_t v;
                    memcpy(&v, buf + k, 8);
                    fprintf(stderr, " %llx", (unsigned long long) v);
                }
                fprintf(stderr, "\n");
                /* The same bytes as text: a service name or path in the payload is what names
                   the peer without decoding the IPC space. */
                fprintf(stderr, "hengevm:   ascii:");
                for (int k = 0x18; k < 0x1b8; k++) {
                    uint8_t c = buf[k];
                    fputc((c >= 32 && c < 127) ? c : '.', stderr);
                }
                fprintf(stderr, "\n");
                shown++;
            }
        }
        {
            int shown = 0;
            HvCpu *cpu = (g_svc_m && g_svc_m->cpus[0]) ? g_svc_m->cpus[0] : NULL;
            if (!cpu) goto g_svc_skip_args;
            for (uint64_t i = n; i-- > 0 && shown < 3; ) {
                uint64_t s = i & (HV_SVC_CAP - 1);
                uint32_t no = (uint32_t) hv_svc_x16[s];
                uint64_t args[2] = { hv_svc_x0[s], hv_svc_x1[s] };
                fprintf(stderr, "hengevm: argdump pc %llx sys %u ttbr0 %llx\n",
                        (unsigned long long) hv_svc_pc[s], no,
                        (unsigned long long) hv_svc_ttbr0[s]);
                for (int a = 0; a < 2; a++) {
                    if (!args[a]) continue;
                    /* Walk in the space the call came from, not the current one. */
                    uint64_t saved_ttbr0 = cpu->ttbr0_el1;
                    cpu->ttbr0_el1 = hv_svc_ttbr0[s];
                    HvWalk w;
                    hv_mmu_walk(cpu, args[a], HV_ACC_READ, 0, &w);
                    cpu->ttbr0_el1 = saved_ttbr0;
                    if (w.fsc) continue;
                    uint8_t buf[0x40];
                    if (hv_phys_read(cpu->m, w.pa, buf, sizeof buf) != 0) continue;
                    fprintf(stderr, "hengevm:   arg%d %llx:", a, (unsigned long long) args[a]);
                    for (int k = 0; k < 0x40; k += 8) {
                        uint64_t v;
                        memcpy(&v, buf + k, 8);
                        fprintf(stderr, " %llx", (unsigned long long) v);
                    }
                    fprintf(stderr, "\n");
                }
                shown++;
            }
        g_svc_skip_args:;
        }
        for (uint64_t i = n; i-- > 0; ) {
            uint64_t s = i & (HV_SVC_CAP - 1);
            uint32_t no = (uint32_t) hv_svc_x16[s];
            if (no == 331 || no == 361 || no == 334 || no == 332) continue;
            fprintf(stderr, "hengevm: last non-exit syscall: pc %llx sys %u x0 %llx x1 %llx"
                            " x2 %llx x3 %llx ret %lld\n",
                    (unsigned long long) hv_svc_pc[s], no,
                    (unsigned long long) hv_svc_x0[s], (unsigned long long) hv_svc_x1[s],
                    (unsigned long long) hv_svc_x2[s], (unsigned long long) hv_svc_x3[s],
                    (long long) hv_svc_ret[s]);
            break;
        }
    }
    fprintf(stderr, "hengevm: last %llu syscalls (oldest first; number is guest x16):\n",
            (unsigned long long) cnt);
    for (uint64_t i = n - cnt; i < n; i++) {
        uint64_t s = i & (HV_SVC_CAP - 1);
        int pending = 0;
        for (int c = 0; c < HV_MAX_CPUS; c++)
            if (hv_svc_pending[c] && hv_svc_last_slot[c] == s) pending = 1;
        fprintf(stderr, "  pc %-12llx sys %-4llu x0 %-11llx x1 %-11llx x2 %-11llx x3 %-11llx ret %-6lld%s%s%s\n",
                (unsigned long long) hv_svc_pc[s], (unsigned long long) hv_svc_x16[s],
                (unsigned long long) hv_svc_x0[s], (unsigned long long) hv_svc_x1[s],
                (unsigned long long) hv_svc_x2[s], (unsigned long long) hv_svc_x3[s],
                (long long) hv_svc_ret[s], pending ? "  <-- NOT RETURNED" : "",
                hv_svc_path[s][0] ? "  " : "", hv_svc_path[s]);
    }
}

void hv_fault_stats_dump(void) {
    static const char *names[] = {"svc", "translation", "permission", "access-flag",
                                  "other-dabort", "iabort", "other"};
    fprobe_init();
    fprintf(stderr, "hengevm: fault probe: exceptions per class, then paths reaching each range "
                    "(trace_ring; needs HV_LOG>=2)\n");
    for (int w = 0; w < fprobe_n; w++)
        fprintf(stderr, "hengevm:   probe%d [0x%llx,0x%llx)\n", w,
                (unsigned long long) fprobe_lo[w], (unsigned long long) fprobe_hi[w]);
    for (int i = 1; i < 7; i++) {
        if (!fstat_n[i]) continue;
        fprintf(stderr, "hengevm:   %-12s n=%-6llu", names[i], (unsigned long long) fstat_n[i]);
        for (int w = 0; w < fprobe_n; w++)
            fprintf(stderr, "  p%d=%-6llu(%5.1f%%)", w,
                    (unsigned long long) fstat_probe[i][w],
                    100.0 * (double) fstat_probe[i][w] / (double) fstat_n[i]);
        fprintf(stderr, "\n");
    }

    /* Which 2 MiB regions the translation/permission paths touched, by block count.
       This is what says where a fault path actually goes, with no dispatcher in the way. */
    uint32_t top[12] = {0}; uint64_t topv[12] = {0};
    for (uint32_t i = 0; i < FHIST_N; i++) {
        uint32_t c = fhist[i];
        if (!c) continue;
        for (int k = 0; k < 12; k++) {
            if (c > top[k]) {
                for (int j = 11; j > k; j--) { top[j] = top[j-1]; topv[j] = topv[j-1]; }
                top[k] = c; topv[k] = FHIST_BASE + ((uint64_t) i << 12);
                break;
            }
        }
    }
    fprintf(stderr, "hengevm: page table at eret (walk of the faulting VA after the handler):\n");
    for (int i = 1; i < 7; i++) {
        if (!ptest_n[i]) continue;
        fprintf(stderr, "hengevm:   %-12s n=%-6llu mapped=%-6llu trans-L3=%-6llu other=%-6llu"
                        " | reached_pmap n=%-6llu mapped=%-6llu trans-L3=%-6llu\n", names[i],
                (unsigned long long) ptest_n[i], (unsigned long long) ptest_ok[i],
                (unsigned long long) ptest_zero[i], (unsigned long long) ptest_other[i],
                (unsigned long long) ptest_pn[i], (unsigned long long) ptest_pok[i],
                (unsigned long long) ptest_pzero[i]);
    }
    fprintf(stderr, "hengevm: fault addresses: total=%llu unique_pages=%llu repeats=%llu (%.2f%%)\n",
            (unsigned long long) far_total, (unsigned long long) far_uniq,
            (unsigned long long) far_repeat,
            far_total ? 100.0 * (double) far_repeat / (double) far_total : 0.0);
    {
        uint32_t top[6] = {0}; uint64_t topv[6] = {0}, tops[6] = {0};
        for (uint32_t i = 0; i < FARHASH; i++) {
            uint32_t c = far_cnt[i];
            if (c < 2) continue;
            for (int k = 0; k < 6; k++) if (c > top[k]) {
                for (int j = 5; j > k; j--) { top[j]=top[j-1]; topv[j]=topv[j-1]; tops[j]=tops[j-1]; }
                top[k]=c; topv[k]=far_pg[i]<<14; tops[k]=far_space[i]<<14; break;
            }
        }
        fprintf(stderr, "hengevm: most repeated fault pages (asid-ish ttbr0: va x count):\n");
        for (int k = 0; k < 6 && top[k]; k++)
            fprintf(stderr, "hengevm:   ttbr0 0x%-11llx va 0x%-10llx x%u\n",
                    (unsigned long long) tops[k], (unsigned long long) topv[k], top[k]);
    }
    fprintf(stderr, "hengevm: fault-path hot 4KiB pages in 0x1ca00000..0x1cc00000:\n");
    for (int k = 0; k < 12 && top[k]; k++)
        fprintf(stderr, "hengevm:   0x%-12llx %llu blocks\n",
                (unsigned long long) topv[k], (unsigned long long) top[k]);
}

void hv_take_exception(HvCpu *cpu, int type, uint32_t esr, uint64_t far, int set_far, uint64_t ret) {
    {
        HvExcRec *r = &hv_exc_ring[hv_exc_at++ & (HV_EXC_RING - 1)];
        r->elr = ret, r->far = far, r->sp = cpu->sp, r->esr = esr;
        r->el = (uint32_t) cpu->el, r->type = (uint32_t) type;
    }
    if (cpu->el == 0) {
        uint64_t k = hv_uexc_n++ & (HV_UEXC_CAP - 1);
        hv_uexc_elr[k] = ret;
        hv_uexc_far[k] = far;
        hv_uexc_pc[k] = cpu->pc;
        hv_uexc_esr[k] = esr;
        hv_uexc_type[k] = (uint32_t) type;
        if (type == HV_EXC_SYNC && ((esr >> 26) & 0x3f) != 0x15) {
            uint64_t fi = hv_el0_flt_n++ & (HV_EL0_FLT - 1);
            hv_el0_flt_pc[fi] = cpu->pc;
            hv_el0_flt_ret[fi] = ret;
            hv_el0_flt_far[fi] = far;
            hv_el0_flt_sp[fi] = cpu->sp;
            hv_el0_flt_esr[fi] = esr;
            hv_el0_flt_ttbr0[fi] = cpu->ttbr0_el1;
            fprobe_init();
            /* Only the first exception taken from EL0 counts: an IPI or a nested same-EL fault
               during the handler must not overwrite it, or the ring we scan at eret is the
               interrupt's path rather than the fault's. */
            if (cpu->el == 0 && flt_class(esr) && cpu->index < HV_MAX_CPUS && !pend_valid[cpu->index]) {
                pend_esr[cpu->index] = esr;
                pend_far[cpu->index] = far;
                pend_ttbr0[cpu->index] = cpu->ttbr0_el1;
                pend_valid[cpu->index] = 1;
            }
            for (int r = 0; r < 31; r++) hv_el0_flt_x[fi][r] = cpu->x[r];
            /* One line per fault is too much: a userland bring-up takes tens of thousands of
               them. The first 64 stay verbatim, then one in 4096, which keeps the shape of the
               run readable. The fault ring keeps every entry regardless. */
            static unsigned long long fault_lines;
            if (cpu->m->log >= 1 && (fault_lines++ < 64 || (hv_el0_flt_n % 4096) == 0)) {
                fprintf(stderr, "hengevm: el0 fault #%llu ec %llx pc %llx far %llx lr %llx sp %llx"
                                " x0 %llx x1 %llx x8 %llx x19 %llx x20 %llx x21 %llx x22 %llx\n",
                        (unsigned long long) hv_el0_flt_n,
                        (unsigned long long) ((esr >> 26) & 0x3f),
                        (unsigned long long) cpu->pc, (unsigned long long) far,
                        (unsigned long long) cpu->x[30], (unsigned long long) cpu->sp,
                        (unsigned long long) cpu->x[0], (unsigned long long) cpu->x[1],
                        (unsigned long long) cpu->x[8], (unsigned long long) cpu->x[19],
                        (unsigned long long) cpu->x[20], (unsigned long long) cpu->x[21],
                        (unsigned long long) cpu->x[22]);
            }
        }
    }
    /* Every exception and interrupt the engine delivers, while diagnosing: a guest that ends up in
       its own vector with no log of how leaves nothing to go on. */
    /* Not the SVCs: a user process making syscalls says nothing, and there is enough of that to
       fill any limit and hide the one exception that matters. */
    /* The last syscalls a stalled userland made say what it was doing when it stopped; a file,
       because there are tens of thousands of them and stderr at this rate buries everything. */
    if (type == HV_EXC_SYNC && ((esr >> 26) == 0x15) && cpu->el == 0) {
        static FILE *sf;
        static int opened;
        if (!opened) {
            opened = 1;
            const char *p = getenv("HENGE_SYSCALL_LOG");
            if (p && *p) sf = fopen(p, "w");
        }
        /* open() carries its path in x0: the last one before a stall names the file the process
           was waiting on. */
        char path[160] = "";
        if (sf && cpu->x[16] == 5 && cpu->x[0]) {
            int n = 0;
            for (; n < 127; n++) {
                HvWalk w; uint8_t ch = 0;
                hv_mmu_walk(cpu, cpu->x[0] + (uint64_t) n, HV_ACC_READ, 0, &w);
                if (w.fsc != 0 || hv_phys_read(cpu->m, w.pa, &ch, 1) != 0 || ch == 0) break;
                path[n] = (char) ch;
            }
            path[n] = 0;
        }
        if (sf) fprintf(sf, "%llu x16 %llu x0 %llx x1 %llx x2 %llx x3 %llx %s\n",
                        (unsigned long long) cpu->icount, (unsigned long long) cpu->x[16],
                        (unsigned long long) cpu->x[0], (unsigned long long) cpu->x[1],
                        (unsigned long long) cpu->x[2], (unsigned long long) cpu->x[3], path);
    }
    if (cpu->m->log >= 3 && (type != HV_EXC_SYNC || (esr >> 26) != 0x15) &&
        cpu->m->stats.exceptions + cpu->m->stats.interrupts < 4096)
        fprintf(stderr, "hengevm: take %s esr %08x far %llx at pc %llx el%u vector %llx\n",
                type == HV_EXC_SYNC ? "sync" : type == HV_EXC_IRQ ? "irq" : "fiq",
                esr, (unsigned long long) far, (unsigned long long) ret, cpu->el,
                (unsigned long long) (cpu->vbar_el1 + (cpu->el ? 0x200 : 0x400)));
    if (type == HV_EXC_SYNC) cpu->m->stats.exceptions++;
    else cpu->m->stats.interrupts++;
    uint64_t spsr = hv_pstate_get(cpu);
    {
        HvStats *st = &cpu->m->stats;
        uint32_t k = st->nelchg++ & 15;
        st->elchg[k].from = ret, st->elchg[k].spsr = spsr, st->elchg[k].kind = (uint32_t)type, st->elchg[k].el = cpu->el;
        st->elchg[k].to = esr;
    }
    if (cpu->m->trace || cpu->m->log >= 2) {
        char extra[64];
        extra[0] = '\0';
        /* A data abort from a lower EL is the interesting one: print the instruction that made
           the access, because "fault at pc X" alone does not say whether the guest wandered into
           bad code or used a bad pointer from good code. The page PC is in is mapped, so it can be
           read through the same walk the TLB uses. */
        /* 0x24 is a data abort from EL0, 0x25 from EL1: an early-boot kernel fault is the
               second, and it is the one worth seeing. */
        if (type == HV_EXC_SYNC && (((esr >> 26) & 0x3f) == 0x24 || ((esr >> 26) & 0x3f) == 0x25)) {
            /* A EL0 fault is the only moment the userland's own page tables are known to be the
               ones installed, so it is the moment to take the page it was executing: the restore
               ramdisk is an APFS container with compressed files in it, so the program cannot be
               read out of the image, but it can be read out of the guest's memory as it runs. */
            if (cpu->el == 0 && getenv("HV_EL0_DUMP")) {
                static int dumped;
                if (dumped < 12) {
                    HvWalk uw;
                    memset(&uw, 0, sizeof uw);
                    hv_mmu_walk(cpu, ret, HV_ACC_READ, 0, &uw);
                    if (uw.fsc == 0) {
                        uint8_t pg[4096];
                        if (hv_phys_read(cpu->m, uw.pa & ~0xfffull, pg, sizeof pg) == 0) {
                            char path[160];
                            snprintf(path, sizeof path, "/data/local/tmp/henge/el0_%02d_%llx_%llx.bin",
                                     dumped, (unsigned long long) ret, (unsigned long long) (uw.pa & ~0xfffull));
                            FILE *f = fopen(path, "wb");
                            if (f) {
                                fwrite(pg, 1, sizeof pg, f);
                                fclose(f);
                                fprintf(stderr, "hengevm: el0 page pc %llx pa %llx -> %s\n",
                                        (unsigned long long) ret, (unsigned long long) uw.pa, path);
                                dumped++;
                            }
                        }
                    }
                }
            }
            HvWalk w;
            hv_mmu_walk(cpu, ret, HV_ACC_READ, 1, &w);
            uint32_t insn = 0;
            /* Always, whatever the walk said: the heavy diagnostics below sit behind conditions
               that a stale TLB can fail, and a fault that prints nothing is worse than one that
               prints only the essentials. */
            if (cpu->m->log >= 2)
                fprintf(stderr, "hengevm: data abort: esr %08x far %llx pc %llx el%u insn-ok %d "
                                "x0 %llx x2 %llx x4 %llx x16 %llx x29 %llx x30 %llx\n",
                        esr, (unsigned long long) far, (unsigned long long) ret, cpu->el,
                        w.fsc == 0, (unsigned long long) cpu->x[0], (unsigned long long) cpu->x[2],
                        (unsigned long long) cpu->x[4], (unsigned long long) cpu->x[16],
                        (unsigned long long) cpu->x[29], (unsigned long long) cpu->x[30]);
            /* A permission fault on a write: the whole descriptor chain says whether the guest
               mapped the page read-only on purpose or this walk is reading the wrong table.
               Nothing used to dump anything for these -- the diagnostics below keyed on
               translation faults -- and these are permission faults. */
            /* An EL0 translation fault in the shared-cache / dylib window: walk the faulting
               address through the guest's own tables. If this walk finds a valid mapping, then
               the guest did map the page and the fault came from our fast path (stale TLB
               entry), which is a different bug from "the page was never mapped". */
            if (((esr >> 26) & 0x3f) == 0x24 && far >= 0x100000000ull && far < 0x110000000ull &&
                cpu->m->log >= 2) {
                static int n;
                if (n < 10) {
                    n++;
                    FILE *saved0 = hv_walk_dbg;
                    HvWalk fw;
                    hv_walk_dbg = stderr;
                    hv_mmu_walk(cpu, far, HV_ACC_READ, 0, &fw);
                    hv_walk_dbg = saved0;
                    fprintf(stderr, "hengevm: el0 fault far %llx walk: fsc 0x%x pa %llx L3 %llx\n",
                            (unsigned long long) far, fw.fsc, (unsigned long long) fw.pa,
                            (unsigned long long) fw.table);
                }
            }
            /* Same reasoning as the fault line above, and this one costs a page walk too. */
            static int perm_shown;
            if ((esr & 0x3c) == 0x0c && cpu->m->log >= 2 && perm_shown++ < 16) {
                FILE *saved = hv_walk_dbg;
                HvWalk fw;
                hv_walk_dbg = stderr;
                hv_mmu_walk(cpu, far, HV_ACC_WRITE, 1, &fw);
                hv_walk_dbg = saved;
                fprintf(stderr, "hengevm: perm walk far %llx: fsc %x pa %llx L3-table %llx\n",
                        (unsigned long long) far, fw.fsc, (unsigned long long) fw.pa,
                        (unsigned long long) fw.table);
            }
            /* A level-0 translation fault is almost always a bad pointer rather than bad code:
               the registers at the fault are the arguments the caller passed, and x30 says who
               the caller was -- which is the only thing that turns "somewhere a pointer went
               wrong" into a place to look. */
            /* Everything below -- the walk, the registers at the fault, the bytes before the
               fault, the image headers -- is a bring-up diagnostic. Left uncapped it is the
               largest block in the log (measured: 35k occurrences of "at the fault" in one
               restore run). The first 32 are worth reading; after that the shape is known. */
            static int fault_diag_shown;
            if ((w.fsc == 0 || (w.fsc & 0x3c) == 0x04) && cpu->m->log >= 2 && fault_diag_shown++ < 32) {
                /* The caller of the function that faulted: x30 is inside it, and [x29+8] is the
                   saved return address of its caller. Without it a fault inside a formatter says
                   nothing about who asked for the formatting. */
                uint64_t lr2 = 0;
                HvWalk w2;
                if (cpu->x[29]) {
                    hv_mmu_walk(cpu, cpu->x[29] + 8, HV_ACC_READ, 1, &w2);
                    (void) hv_phys_read(cpu->m, w2.pa, &lr2, 8);
                }
                /* The varargs of a printf live on the stack, and a %s whose argument was never
                   passed reads one of these slots: the stack around sp says what the formatter
                   was actually given, which the registers at the fault no longer do. */
                /* The panic path reads its "caller" from a global and its reason from the
                   argument list; both are what the message would have said, and the message is
                   the only thing that says why the kernel stopped. */
                uint64_t caller2 = 0, x24v = cpu->x[24], x26v = cpu->x[26], glob = 0;
                HvWalk w4;
                hv_mmu_walk(cpu, x26v + 0x794, HV_ACC_READ, 1, &w4);
                (void) hv_phys_read(cpu->m, w4.pa, &glob, 4);
                /* panic() records what it is about to report before it reports it: the cpu,
                   the caller and the reason string, in globals around the base the panic
                   printer loads with adrp. They are still there while the report is failing,
                   and they are the only surviving account of why the kernel stopped. */
                uint64_t pg = 0xfffffe001f22a000ull, gv[6];
                for (int k = 0; k < 6; k++) {
                    gv[k] = 0;
                    hv_mmu_walk(cpu, pg + 0x790 + 8u * (uint64_t) k, HV_ACC_READ, 1, &w2);
                    (void) hv_phys_read(cpu->m, w2.pa, &gv[k], 8);
                }
                /* The call chain into panic is on the stack, not in a register: each frame's
                   saved return address is at [x29+8] and the previous x29 at [x29]. Walking it
                   says who asked for the report, which no global did. */
                uint64_t chain[10];
                uint64_t fp = cpu->x[29];
                for (int k = 0; k < 10; k++) {
                    chain[k] = 0;
                    if (!fp || (fp & 7) || (fp >> 48) != 0xffff) break;
                    hv_mmu_walk(cpu, fp + 8, HV_ACC_READ, 1, &w2);
                    if (hv_phys_read(cpu->m, w2.pa, &chain[k], 8)) break;
                    uint64_t prev = 0;
                    hv_mmu_walk(cpu, fp, HV_ACC_READ, 1, &w2);
                    if (hv_phys_read(cpu->m, w2.pa, &prev, 8)) break;
                    if (prev <= fp) break;   /* a frame must be above the one that called it */
                    fp = prev;
                }
                uint64_t stack[12];
                HvWalk w3;
                for (int k = 0; k < 12; k++) {
                    stack[k] = 0;
                    hv_mmu_walk(cpu, cpu->sp + 8u * (uint64_t) k, HV_ACC_READ, 1, &w3);
                    (void) hv_phys_read(cpu->m, w3.pa, &stack[k], 8);
                }
                static int n0;
                if (n0++ < 4)
                    fprintf(stderr, "hengevm: lvl0 fault fsc %x far %llx pc %llx lr %llx "
                                    "x0 %llx x1 %llx x2 %llx x3 %llx x4 %llx x5 %llx x6 %llx "
                                    "x7 %llx x19 %llx x20 %llx x25 %llx x27 %llx x29 %llx caller %llx\n",
                            (unsigned) w.fsc, (unsigned long long) far, (unsigned long long) ret,
                            (unsigned long long) cpu->x[30],
                            (unsigned long long) cpu->x[0], (unsigned long long) cpu->x[1],
                            (unsigned long long) cpu->x[2], (unsigned long long) cpu->x[3],
                            (unsigned long long) cpu->x[4], (unsigned long long) cpu->x[5],
                            (unsigned long long) cpu->x[6], (unsigned long long) cpu->x[7],
                            (unsigned long long) cpu->x[19], (unsigned long long) cpu->x[20],
                            (unsigned long long) cpu->x[25], (unsigned long long) cpu->x[27],
                            (unsigned long long) cpu->x[29], (unsigned long long) lr2);
                if (n0 <= 4)
                    fprintf(stderr, "hengevm:   chain %llx %llx %llx %llx %llx %llx %llx "
                                    "%llx %llx %llx\n",
                            (unsigned long long) chain[0], (unsigned long long) chain[1],
                            (unsigned long long) chain[2], (unsigned long long) chain[3],
                            (unsigned long long) chain[4], (unsigned long long) chain[5],
                            (unsigned long long) chain[6], (unsigned long long) chain[7],
                            (unsigned long long) chain[8], (unsigned long long) chain[9]);
                if (n0 <= 4)
                    fprintf(stderr, "hengevm:   panic globals 790 %llx 798 %llx 7a0 %llx "
                                    "7a8 %llx 7b0 %llx 7b8 %llx\n",
                            (unsigned long long) gv[0], (unsigned long long) gv[1],
                            (unsigned long long) gv[2], (unsigned long long) gv[3],
                            (unsigned long long) gv[4], (unsigned long long) gv[5]);
                if (n0 <= 4)
                    fprintf(stderr, "hengevm:   x24 %llx x26 %llx glob794 %llx\n",
                            (unsigned long long) x24v, (unsigned long long) x26v,
                            (unsigned long long) (glob & 0xffffffffu));
                if (n0 <= 4)
                    for (int k = 0; k < 6; k++) {
                        if ((gv[k] >> 48) != 0xffff || (gv[k] & 7)) continue;
                        char msg[65];
                        int j;
                        for (j = 0; j < 64; j++) {
                            uint64_t ch = 0;
                            hv_mmu_walk(cpu, gv[k] + (uint64_t) j, HV_ACC_READ, 1, &w2);
                            if (hv_phys_read(cpu->m, w2.pa, &ch, 1)) break;
                            ch &= 0xff;
                            if (!ch) break;
                            msg[j] = (char) ((ch >= 32 && ch < 127) ? ch : '.');
                        }
                        msg[j] = 0;
                        if (j > 2)
                            fprintf(stderr, "hengevm:   glob+%x -> \"%s\"\n",
                                    0x790 + 8 * k, msg);
                    }
                if (n0 <= 4)
                    fprintf(stderr, "hengevm:   stack %llx %llx %llx %llx %llx %llx "
                                    "%llx %llx %llx %llx %llx %llx\n",
                            (unsigned long long) stack[0], (unsigned long long) stack[1],
                            (unsigned long long) stack[2], (unsigned long long) stack[3],
                            (unsigned long long) stack[4], (unsigned long long) stack[5],
                            (unsigned long long) stack[6], (unsigned long long) stack[7],
                            (unsigned long long) stack[8], (unsigned long long) stack[9],
                            (unsigned long long) stack[10], (unsigned long long) stack[11]);
            }
            if (w.fsc == 0 && hv_phys_read(cpu->m, w.pa, &insn, 4) == 0) {
                snprintf(extra, sizeof extra, " insn %08x", insn);
                /*
                 * The registers at the fault, not at the panic. The guest runs its own handler
                 * before it reports, and that handler is free to use x0-x8: the dump in its panic
                 * report is not the faulting state, which is why it disagreed with the code that
                 * built the address.
                 */
                /* Ring trace + helper histogram + the executed path. On a userland fault the
                   "path from block ... to fault" line alone is up to a megabyte, and there are
                   many such faults, so this is debug-only (HV_LOG=3). */
                if (cpu->m->log >= 3) {
                    hv_trace_ring_dump(cpu);
                    hv_helper_hist_dump(cpu);
                    /*
                     * The code at each of the last distinct block starts. The trace says where the
                     * guest was; the instructions there say what it was doing — and a loop that
                     * corrupts a register shows as several blocks cycling, which reads very
                     * differently from a straight line.
                     */
                    uint64_t seen[8];
                    int nseen = 0;
                    for (int i = 255; i >= 0 && nseen < 8; i--) {
                        uint64_t va = cpu->trace_ring[((cpu->trace_idx + (uint32_t) i) & 255) * 4];
                        int dup = 0;
                        for (int k = 0; k < nseen; k++) if (seen[k] == va) dup = 1;
                        if (dup || !va) continue;
                        seen[nseen++] = va;
                        fprintf(stderr, "hengevm: block %llx:", (unsigned long long) va);
                        for (int w = 0; w < 56; w++) {
                            HvWalk bww;
                            uint32_t word = 0;
                            hv_mmu_walk(cpu, va + (uint64_t) w * 4, HV_ACC_READ, 0, &bww);
                            if (bww.fsc == 0 && hv_phys_read(cpu->m, bww.pa, &word, 4) == 0)
                                fprintf(stderr, " %08x", word);
                            else
                                fprintf(stderr, " ????????");
                        }
                        fprintf(stderr, "\n");
                    }
                }
                /*
                 * The header of whatever image the firmware is decoding, found by its magic. The
                 * ROM decompresses the iBSS it received over DFU, so if that image is intact at its
                 * header the fault is in how it is decoded; if it is shifted or absent, the fault is
                 * in the transfer — and the two need different fixes.
                 */
                /* Scanning 64 MB of guest RAM per fault is the most expensive diagnostic in the
                   engine (measured: 22,649 scans in one restore run), so it keeps a budget of
                   its own, smaller than the block above. */
                static int bvx2_shown;
                if (bvx2_shown++ < 4) {
                    /*
                     * What the firmware is decoding. The ROM decompresses the iBSS it received, and
                     * the container is not necessarily IM4P: what matters is whether the stream
                     * begins where the decoder says it does, so the magics of both formats Apple
                     * uses are looked for, and the bytes at the decoder's own position are printed.
                     */
                    HvRam *ram = hv_ram_lookup(cpu->m, 0x70000000ull);
                    uint8_t *base = ram ? ram->host : NULL;
                    size_t span = ram ? ram->size : 0;
                    static const char *const magics[] = {"bvx2", "bvxn", "IM4P", "IM4M"};
                    int found = 0;
                    size_t limit = span < (64u << 20) ? span : (64u << 20);
                    for (size_t off = 0; base && off + 4 < limit && found < 6; off += 4) {
                        for (unsigned k = 0; k < sizeof magics / sizeof magics[0]; k++) {
                            if (memcmp(base + off, magics[k], 4) != 0) continue;
                            fprintf(stderr, "hengevm: %s at ram+%#zx: ", magics[k], off);
                            for (int b = 0; b < 24 && off + (size_t) b < span; b++)
                                fprintf(stderr, "%02x", base[off + (size_t) b]);
                            fprintf(stderr, "\n");
                            found++;
                            break;
                        }
                    }
                    if (!found) fprintf(stderr, "hengevm: no bvx2/bvxn/IM4P/IM4M in the first 64 MB\n");
                    fprintf(stderr, "hengevm: diag end\n");
                    /* The bytes the decoder is currently reading. */
                    if (base && cpu->x[2] >= 0x70000000ull && cpu->x[2] < 0x70000000ull + span) {
                        size_t at = (size_t)(cpu->x[2] - 0x70000000ull) & ~(size_t)0x3f;
                        fprintf(stderr, "hengevm: at x2 %llx (%#zx): ",
                                (unsigned long long) cpu->x[2], at);
                        for (int b = 0; b < 48 && at + (size_t) b < span; b++)
                            fprintf(stderr, "%02x", base[at + (size_t) b]);
                        fprintf(stderr, "\n");
                    }
                }
                /* One line per fault, tens of thousands of them in a bring-up run. */
                static int atfault_shown;
                if (atfault_shown++ < 32)
                fprintf(stderr, "hengevm: at the fault: x0=%llx x1=%llx x2=%llx x3=%llx "
                                "x4=%llx x5=%llx x6=%llx x7=%llx\n",
                        (unsigned long long) cpu->x[0], (unsigned long long) cpu->x[1],
                        (unsigned long long) cpu->x[2], (unsigned long long) cpu->x[3],
                        (unsigned long long) cpu->x[4], (unsigned long long) cpu->x[5],
                        (unsigned long long) cpu->x[6], (unsigned long long) cpu->x[7]);
                /*
                 * The word the guest's 32-bit load was supposed to bring in, read with the *fault
                 * time* context (el 0) rather than the kernel context the panic handler runs in.
                 * It settles the only question left: whether the memory held the expected value
                 * and the translation dropped it, or the memory itself held a host address.
                 */
                {
                    /* This probe was written for one specific EL0 fault; running it on every
                       fault during a userland bring-up costs a page walk plus a line of stderr
                       per page-in, which dominates the run.  Keep it behind an explicit switch
                       (or -v log level 3) and cap it, so a normal run pays nothing. */
                    static int shown;
                    if ((cpu->m->log >= 3 || getenv("HV_EL0_DUMP")) && shown++ < 8) {
                        uint64_t at = cpu->x[0] + 0x1C0, word = 0;
                        HvWalk ww;
                        hv_mmu_walk(cpu, at, HV_ACC_READ, 0, &ww);
                        if (ww.fsc == 0 && hv_phys_read(cpu->m, ww.pa, &word, 8) == 0)
                            fprintf(stderr, "hengevm: [x0+0x1c0] = %016llx (pa %llx)\n",
                                    (unsigned long long) word, (unsigned long long) ww.pa);
                        else
                            fprintf(stderr, "hengevm: [x0+0x1c0] unreadable (fsc %u)\n", ww.fsc);
                    }
                }
                /*
                 * The executed path, not the bytes before it. A linear window before the fault
                 * mixes in branches that were not taken — the load that put the bad value in the
                 * base register is then invisible, and the window says the wrong thing. The last
                 * block this CPU entered starts the path; from there every word up to the fault
                 * is an instruction that ran.
                 */
                uint64_t from = hv_pc_hist_last(cpu->index);
                if (!from || from > ret) from = ret - 32 * 4;
                /* The executed path can be thousands of words -- a single line of up to a
                   megabyte per fault, and userland faults are frequent. Debug-only. */
                if (cpu->m->log >= 3) {
                    fprintf(stderr, "hengevm: path from block %llx to fault %llx:",
                            (unsigned long long) from, (unsigned long long) ret);
                    for (uint64_t at = from; at <= ret; at += 4) {
                        HvWalk ww;
                        uint32_t word = 0;
                        hv_mmu_walk(cpu, at, HV_ACC_READ, 1, &ww);
                        if (ww.fsc == 0 && hv_phys_read(cpu->m, ww.pa, &word, 4) == 0)
                            fprintf(stderr, " %08x", word);
                        else
                            fprintf(stderr, " ????????");
                    }
                    fprintf(stderr, "\n");
                }
                /*
                 * The words the guest was reading from. A host address inside guest-visible
                 * memory is the answer to "who put it there": it cannot come from the guest.
                 */
                {
                    uint64_t base = cpu->x[0];
                    fprintf(stderr, "hengevm: guest memory at x0 %llx:\n",
                            (unsigned long long) base);
                    for (int row = 0; row < 8; row++) {
                        fprintf(stderr, "  %llx:", (unsigned long long) (base + (uint64_t) row * 32));
                        for (int k = 0; k < 4; k++) {
                            uint64_t at = base + (uint64_t) row * 32 + (uint64_t) k * 8, word = 0;
                            HvWalk ww;
                            hv_mmu_walk(cpu, at, HV_ACC_READ, 1, &ww);
                            if (ww.fsc == 0 && hv_phys_read(cpu->m, ww.pa, &word, 8) == 0)
                                fprintf(stderr, " %016llx", (unsigned long long) word);
                            else
                                fprintf(stderr, " ????????????????");
                        }
                        fprintf(stderr, "\n");
                    }
                }
                /* Where x4 points, if it is a host address: the words there say what kind of
                   object it is (a GOT slot, a JIT literal, a C stack frame). */
                {
                    uint64_t v = cpu->x[4];
                    fprintf(stderr, "hengevm: x4 = %llx, words there:",
                            (unsigned long long) v);
                    for (int k = 0; k < 4; k++) {
                        HvWalk ww;
                        uint32_t word = 0;
                        hv_mmu_walk(cpu, v + (uint64_t) k * 4, HV_ACC_READ, 1, &ww);
                        if (ww.fsc == 0 && hv_phys_read(cpu->m, ww.pa, &word, 4) == 0)
                            fprintf(stderr, " %08x", word);
                        else
                            fprintf(stderr, " ????????");
                    }
                    fprintf(stderr, "\n");
                }
                uint64_t here = (uint64_t)(uintptr_t) &hv_take_exception;
                uint64_t jit_base = (uint64_t)(uintptr_t) cpu->m->jit.base;
                uint64_t jit_end = jit_base + cpu->m->jit.capacity * 4;
                fprintf(stderr, "hengevm: jit rx %llx..%llx, self %llx\n",
                        (unsigned long long) jit_base, (unsigned long long) jit_end,
                        (unsigned long long) here);
                for (int r = 0; r < 31; r++) {
                    uint64_t v = cpu->x[r];
                    uint64_t d = v > here ? v - here : here - v;
                    if (v && d < (64ull << 20))
                        fprintf(stderr, "hengevm: x%d = host %llx (%s0x%llx)\n", r,
                                (unsigned long long) v, v > here ? "+" : "-",
                                (unsigned long long) d);
                }
            }
        }
        /* A data abort taken by kernel code is where the guest's own bookkeeping has already
           gone wrong, so the registers are the only remaining evidence of how. Four of them
           are enough to see the pattern; the rest are the same fault over and over. */
        /* The full data-abort dump (registers, zone, executed path, page tables) is a debug
           aid, not something a normal log level should pay for. */
        if (type == HV_EXC_SYNC && ((esr >> 26) & 0x3f) == 0x25 && cpu->m->log >= 3) {
            static int el1_dabt;
            if (el1_dabt < 4) {
                el1_dabt++;
                fprintf(stderr, "hengevm: el1 data abort, registers:");
                for (int r = 0; r < 31; r++) {
                    if ((r % 6) == 0) fprintf(stderr, "\nhengevm:  ");
                    fprintf(stderr, " x%d=%llx", r, (unsigned long long) cpu->x[r]);
                }
                /* Which zone the faulting code was allocating from: the caller loads the zone
                   pointer from a kernel global, and a zone carries its own name, so the string
                   says what kind of object this was without any symbol table. */
                {
                    uint64_t knob = 0xfffffe001c335440ull;
                    HvWalk kw; uint64_t zone = 0;
                    hv_mmu_walk(cpu, knob, HV_ACC_READ, 1, &kw);
                    if (kw.fsc == 0 && hv_phys_read(cpu->m, kw.pa, &zone, 8) == 0) {
                        fprintf(stderr, "\nhengevm:  zone global %llx -> %#llx\n",
                                (unsigned long long) knob, (unsigned long long) zone);
                        for (int k = 0; k < 24; k++) {
                            HvWalk rw; uint64_t raw = 0;
                            hv_mmu_walk(cpu, zone + (uint64_t) k * 8, HV_ACC_READ, 1, &rw);
                            if (rw.fsc == 0 && hv_phys_read(cpu->m, rw.pa, &raw, 8) == 0)
                                fprintf(stderr, "hengevm:   z[%02d] %016llx\n", k, (unsigned long long) raw);
                        }
                        for (int k = 0; k < 64; k++) {
                            HvWalk zw; uint64_t p = 0;
                            hv_mmu_walk(cpu, zone + (uint64_t) k * 8, HV_ACC_READ, 1, &zw);
                            if (zw.fsc != 0 || hv_phys_read(cpu->m, zw.pa, &p, 8) != 0) continue;
                            if (p < 0xfffffe0000000000ull || p > 0xffffffffffffffffull) continue;
                            char txt[41];
                            int ok = 1;
                            for (int i = 0; i < 40; i += 8) {
                                HvWalk tw; uint64_t w = 0;
                                hv_mmu_walk(cpu, p + (uint64_t) i, HV_ACC_READ, 1, &tw);
                                if (tw.fsc != 0 || hv_phys_read(cpu->m, tw.pa, &w, 8) != 0) { ok = 0; break; }
                                memcpy(txt + i, &w, 8);
                            }
                            txt[40] = 0;
                            for (int i = 0; i < 40 && ok; i++)
                                if (txt[i] && (txt[i] < 0x20 || txt[i] > 0x7e)) ok = 0;
                            if (ok && txt[0]) fprintf(stderr, "hengevm:   zone+%#04x -> %llx \"%s\"\n",
                                                      k * 8, (unsigned long long) p, txt);
                        }
                    }
                }
                /* The block that faulted, word by word: the load that made the access is in it,
                   and so is whatever advanced the base register to the bad address. */
                {
                    uint64_t from = hv_pc_hist_last(cpu->index);
                    if (!from || from > ret) from = ret - 32 * 4;
                    fprintf(stderr, "\nhengevm:  executed from %llx:", (unsigned long long) from);
                    for (uint64_t at = from; at <= ret; at += 4) {
                        HvWalk ww; uint32_t word = 0;
                        hv_mmu_walk(cpu, at, HV_ACC_READ, 1, &ww);
                        if (ww.fsc == 0 && hv_phys_read(cpu->m, ww.pa, &word, 4) == 0)
                            fprintf(stderr, " %08x", word);
                        else fprintf(stderr, " ????????");
                    }
                    fprintf(stderr, "\n");
                }
                /* Whether the guest's own tables map what it just touched: the descriptors say
                   if the page is missing (the guest's data is wrong) or the walk went somewhere
                   else (this engine's translation is wrong). */
                {
                    /* The state the guest's handler will see: whether it may call vm_fault for a
                   pageable kernel page at all depends on the interrupt mask in this PSTATE. */
                fprintf(stderr, "\nhengevm:  spsr %llx (I=%llu F=%llu) daif %llx el %u\n",
                        (unsigned long long) spsr, (unsigned long long) ((spsr >> 7) & 1),
                        (unsigned long long) ((spsr >> 6) & 1), (unsigned long long) cpu->daif, cpu->el);
                fprintf(stderr, "hengevm:  ttbr0=%llx ttbr1=%llx tcr=%llx\n",
                            (unsigned long long) cpu->ttbr0_el1, (unsigned long long) cpu->ttbr1_el1,
                            (unsigned long long) cpu->tcr_el1);
                    FILE *saved = hv_walk_dbg;
                    hv_walk_dbg = stderr;
                    HvWalk w0;
                    hv_mmu_walk(cpu, cpu->x[0], HV_ACC_READ, 1, &w0);
                    fprintf(stderr, "hengevm:  x0 walk: fsc %u pa %llx\n",
                            w0.fsc, (unsigned long long) w0.pa);
                    /* The table the walk stopped in, entry by entry: a table full of valid
                       descriptors with one zero is a page the kernel meant to map and did not;
                       an empty table means the address itself is not one the kernel allocated. */
                    if (w0.fsc == HV_FSC_TRANSLATION(3) && w0.table) {
                        int filled = 0, first = -1, last = -1;
                        for (int k = 0; k < 512; k++) {
                            uint64_t e = 0;
                            if (hv_phys_read(cpu->m, w0.table + (uint64_t) k * 8, &e, 8) != 0) continue;
                            if (e) { filled++; if (first < 0) first = k; last = k; }
                        }
                        fprintf(stderr, "hengevm:  L3 table %llx: %d of 512 entries set, first %d last %d\n",
                                (unsigned long long) w0.table, filled, first, last);
                        /* Which pages, and with what: five entries in a 2 MB window is a small
                           allocation mapped on demand, and the shape says whether the address
                           being touched is inside it (a missing PTE) or past its end (a wrong
                           pointer), which are different bugs. */
                        for (int k = 0; k < 512; k++) {
                            uint64_t e = 0;
                            if (hv_phys_read(cpu->m, w0.table + (uint64_t) k * 8, &e, 8) != 0) continue;
                            if (e) fprintf(stderr, "hengevm:   [%3d] +%#07x %016llx\n", k, k * 0x1000,
                                           (unsigned long long) e);
                        }
                    }
                    hv_walk_dbg = saved;
                    for (int k = 0; k < 2; k++) {
                        uint64_t base = k ? cpu->x[20] : cpu->x[19];
                        fprintf(stderr, "hengevm:  at x%d %llx:", k ? 20 : 19, (unsigned long long) base);
                        for (int w = 0; w < 8; w++) {
                            HvWalk ww; uint64_t word = 0;
                            hv_mmu_walk(cpu, base + (uint64_t) w * 8, HV_ACC_READ, 1, &ww);
                            if (ww.fsc == 0 && hv_phys_read(cpu->m, ww.pa, &word, 8) == 0)
                                fprintf(stderr, " %016llx", (unsigned long long) word);
                            else fprintf(stderr, " ????????????????");
                        }
                        fprintf(stderr, "\n");
                    }
                }
                fprintf(stderr, "\nhengevm:  sp_el0=%llx sp_el1=%llx sp=%llx far=%llx\n",
                        (unsigned long long) cpu->sp_el0, (unsigned long long) cpu->tpidr_el1,
                        (unsigned long long) cpu->sp, (unsigned long long) far);
            }
        }
        /* Every exception (interrupt or syscall) lands here, so this is by far the highest
           volume line in the log -- 77k lines on a single restore run. Keep it for HV_LOG=3. */
        if (cpu->m->log >= 3) {
            fprintf(stderr, "exception type %d esr %08x far %llx from pc %llx el%u%s\n", type, esr,
                    (unsigned long long)far, (unsigned long long)ret, cpu->el, extra);
        }
    }
    uint64_t offset = cpu->el == 0 ? 0x400 : cpu->spsel ? 0x200 : 0x000;
    cpu->spsr_el1 = spsr;
    cpu->elr_el1 = ret;
    if (type == HV_EXC_SYNC || type == HV_EXC_SERROR) cpu->esr_el1 = esr;
    if (set_far) cpu->far_el1 = far;
    hv_sp_switch(cpu, 1, 1);
    cpu->daif = HV_PSTATE_DAIF;
    uint64_t extra = cpu->pstate_extra;
    if (!((cpu->sctlr_el1 >> 23) & 1)) extra |= HV_PSTATE_PAN; /* SCTLR_EL1.SPAN == 0 */
    extra &= ~(1ull << 12);
    if ((cpu->sctlr_el1 >> 44) & 1) extra |= 1ull << 12;      /* SSBS from DSSBS */
    set_extra(cpu, extra);
    cpu->excl_addr = ~0ull;
    cpu->pc = (cpu->vbar_el1 & ~0x7ffull) + offset + (uint64_t)type * 0x80;
    cpu->exit_reason = HV_EXIT_EXCEPTION;
}

void hv_raise_sync(HvCpu *cpu, uint32_t ec, uint32_t iss, uint64_t far, uint64_t pc) {
    int set_far = ec == HV_EC_PC_ALIGN || ec == HV_EC_IABT_LOWER || ec == HV_EC_IABT_SAME ||
                  ec == HV_EC_DABT_LOWER || ec == HV_EC_DABT_SAME;
    hv_take_exception(cpu, HV_EXC_SYNC, ec << 26 | 1u << 25 | (iss & 0x1ffffff), far, set_far, pc);
}

void hv_raise_undef(HvCpu *cpu, uint64_t pc) {
    static int reported;
    if (cpu->el == 0 && ((pc >> 55) & 1) && reported++ < 3) hv_debug_el0_in_kernel(cpu, "undef");
    if (cpu->m->log) {
        uint32_t insn = 0;
        uint64_t pa;
        if (!hv_mmu_access(cpu, pc, HV_ACC_EXEC, cpu->el, &pa)) hv_phys_read(cpu->m, pa, &insn, 4);
        fprintf(stderr, "undefined: pc %llx insn %08x el%u\n", (unsigned long long)pc, insn, cpu->el);
        if (cpu->el == 1) { /* a kernel assert: its backtrace, from the frame pointers (lr first) */
            fprintf(stderr, "  backtrace: lr %llx", (unsigned long long)(cpu->x[30] | 0xffffff8000000000ull));
            uint64_t fp = cpu->x[29];
            for (int i = 0; i < 24 && fp; i++) {
                uint64_t fr[2], fpa;
                if (hv_mmu_access(cpu, fp, HV_ACC_READ, 1, &fpa) || hv_phys_read(cpu->m, fpa, fr, 16)) break;
                uint64_t ret = fr[1] | 0xffffff8000000000ull; /* strip the PAC bits */
                fprintf(stderr, " %llx", (unsigned long long)ret);
                if (fr[0] <= fp) break;
                fp = fr[0];
            }
            fprintf(stderr, "\n  vbar %llx\n", (unsigned long long)cpu->vbar_el1);
        }
    }
    hv_raise_sync(cpu, HV_EC_UNKNOWN, 0, 0, pc);
}

void hv_raise_abort(HvCpu *cpu, int insn, uint64_t va, uint32_t fsc, int wnr, int s1ptw, uint64_t pc) {
    uint32_t ec = insn ? (cpu->el ? HV_EC_IABT_SAME : HV_EC_IABT_LOWER)
                       : (cpu->el ? HV_EC_DABT_SAME : HV_EC_DABT_LOWER);
    uint32_t iss = fsc | (uint32_t)(wnr && !insn) << 6 | (uint32_t)s1ptw << 7;
    hv_raise_sync(cpu, ec, iss, va, pc);
}

void hv_eret(HvCpu *cpu) {
    /* The handler has run (we are returning to EL0): the ring now holds its blocks. */
    if (cpu->index < HV_MAX_CPUS && pend_valid[cpu->index] && (cpu->spsr_el1 & 0xf) == 0) {
        uint32_t esr = pend_esr[cpu->index];
        int cls = flt_class(esr);
        pend_valid[cpu->index] = 0;
        if (cls) {
            fstat_n[cls]++;
            int hit0 = 0;
            for (int w = 0; w < fprobe_n; w++) if (ring_has(cpu, w)) { fstat_probe[cls][w]++; if (!w) hit0 = 1; }
            {
                HvWalk w;
                /* Walk in the address space the fault came from: the handler may have switched
                   TTBR0, and walking the current one answers for a different process. */
                uint64_t saved_ttbr0 = cpu->ttbr0_el1;
                cpu->ttbr0_el1 = pend_ttbr0[cpu->index];
                hv_mmu_walk(cpu, pend_far[cpu->index], HV_ACC_READ, 0, &w);
                cpu->ttbr0_el1 = saved_ttbr0;
                ptest_n[cls]++;
                int kind = w.fsc == 0 ? 0 : (w.fsc == HV_FSC_TRANSLATION(3) ? 1 : 2);
                if (kind == 0) ptest_ok[cls]++;
                else if (kind == 1) ptest_zero[cls]++;
                else ptest_other[cls]++;
                {
                    /* Key on (address space, page): the same VA in two processes is a different
                       fault and must not look like a repeat. */
                    uint64_t space = (cpu->ttbr0_el1 & 0x0000ffffffffffffull) >> 14;
                    uint64_t pg = pend_far[cpu->index] >> 14;
                    uint64_t key = pg ^ (space * 0x9e3779b97f4a7c15ull);
                    uint64_t h = (key * 0x9e3779b97f4a7c15ull >> 48) & (FARHASH - 1);
                    if (far_seen[h] == key + 1) { far_repeat++; if (far_cnt[h] < 65535) far_cnt[h]++; }
                    else { far_seen[h] = key + 1; far_pg[h] = pg; far_space[h] = space; far_cnt[h] = 1; far_uniq++; }
                    far_total++;
                }
                if (hit0) {
                    ptest_pn[cls]++;
                    if (kind == 0) ptest_pok[cls]++;
                    else if (kind == 1) ptest_pzero[cls]++;
                }
            }
            if (cls == 1 || cls == 2) {
                for (uint32_t n = 0; n < 256; n++) {
                    uint64_t va = cpu->trace_ring[((cpu->trace_idx + n) & 255) * 4];
                    if (va >= FHIST_BASE && va < FHIST_BASE + ((uint64_t) FHIST_N << 12))
                        fhist[(va - FHIST_BASE) >> 12]++;
                }
            }
        }
    }
    if (cpu->m) {
        HvStats *st = &cpu->m->stats;
        uint32_t k = st->nelchg++ & 15;
        st->elchg[k].from = cpu->pc, st->elchg[k].to = cpu->elr_el1, st->elchg[k].spsr = cpu->spsr_el1;
        st->elchg[k].kind = 9, st->elchg[k].el = cpu->el;
    }
    cpu->pc = cpu->elr_el1;
    hv_pstate_set(cpu, cpu->spsr_el1);
    cpu->excl_addr = ~0ull;
}

static HvCpu *cpu_by_mpidr(HvMachine *m, uint64_t mpidr) {
    for (int i = 0; i < m->ncpus; i++)
        if (m->cpus[i] && (m->cpus[i]->mpidr & 0xff00ffffffull) == (mpidr & 0xff00ffffffull)) return m->cpus[i];
    return NULL;
}

/* PSCI 1.0. */
unsigned long long hv_hvc_count, hv_hvc_last_fn, hv_hvc_last_pc;

/* vmapple's own hypercalls: the CPU service range (PAC keys) and the OEM service range (absolute
   time offset, boot session UUID, vCPU kick). The kernel sets its PAC keys from assembly before it
   can print anything, and that code spins on a non-zero return -- `cbnz x0, .` in pac_asm.h -- so a
   call that comes back unsupported is a silent hang with no console output at all. HengeVM's PAC is
   a no-op, so the key calls only have to report success.
   Return values follow the firmware ABI the kernel checks: x0 >= 0 means success. */
/* The vmapple OEM hypercall range (UID/REVISION/FEATURES, MABS offset, boot session UUID,
   the PV ticket-lock WFK/VCPU_KICK). The kernel only needs an answer if it asks; a guest that
   gets no answer behaves as it would on a hypervisor without OEM support, which is the normal
   path and what this engine runs by default. HENGE_OEM=1 answers them, for a comparison run. */
static int vmapple_hvc(HvCpu *cpu, uint32_t fn) {
    if (!getenv("HENGE_OEM")) return 0;
    switch (fn) {
    /* The PAC keys (0xc100000x) are handled by psci() below: that block predates this one and
       also fills in the default key ids, so there is only one implementation of them. */
    case 0x8100ff01:   /* UID, CPU range */
    case 0x8300ff01:   /* UID, OEM range: must equal VMAPPLE_HVC_UID or the kernel drops every
                          OEM call as unavailable. Words are the UUID read back to front. */
        cpu->x[0] = 0x9cbb6ebb;
        cpu->x[1] = 0x9dc9d679;
        cpu->x[2] = 0xaa624e1f;
        cpu->x[3] = 0x3b878185;
        return 1;
    case 0x8100ff03:   /* REVISION: the OEM table needs major 1 */
    case 0x8300ff03:
        cpu->x[0] = 1;
        cpu->x[1] = 0;
        return 1;
    case 0x8100feff:   /* FEATURES: supported, no flags */
    case 0x8300feff:
        cpu->x[0] = 0;
        cpu->x[1] = 0;
        return 1;
    case 0xc3000003:   /* GET_MABS_OFFSET: x1 in and out. Our timebase is already virtual. */
        cpu->x[0] = 0;
        return 1;
    case 0xc3000004: { /* GET_BOOTSESSIONUUID: reported, never used for anything the guest does */
        cpu->x[0] = 0;
        cpu->x[1] = 0x4f1a2c3d8b6e5d4cull;
        cpu->x[2] = 0x3a2b1c0d6e5d4c3bull;
        return 1;
    }
    case 0xc3000006: { /* VCPU_KICK: the kernel released a ticket lock and wakes the vcpu that is
                          waiting for that ticket (osfmk/arm64/lock_ticket_pv.c). The id is the
                          topology's phys_id, which for us is the cpu index. */
        static int off = -1;
        if (off < 0) off = getenv("HV_NO_PVLOCK") != NULL;
        if (off) return 0;   /* the pre-PV behaviour: not handled, the kernel sees a failure */
        uint64_t id = (uint32_t) cpu->x[1];
        cpu->x[0] = 0;
        /* m->cpus is an array of pointers, so the target is cpus[id], not &cpus[id]. The latter
           compiled with only a warning (-w hides it here) and made t point at the slot instead of
           the CPU: t->pc_state then read whatever the neighbouring slots contained, and a kick to
           any id other than 0 died inside the host as SIGSEGV right after "hvc #28 fn c3000006". */
        if (id < (uint64_t) cpu->m->ncpus && cpu->m->cpus[id]) {
            HvCpu *t = cpu->m->cpus[id];
            pthread_mutex_lock(&t->pc_state->wake_mu);
            t->pc_state->wake_pending = 1;
            pthread_cond_signal(&t->pc_state->wake_cv);
            pthread_mutex_unlock(&t->pc_state->wake_mu);
        }
        return 1;
    }
    case 0xc3000005:   /* VCPU_WFK: hw_lck_ticket_lock_wait_pv() halted this vcpu with interrupts
                          disabled and expects to be parked until a kick or an interrupt arrives,
                          then to get the mask it passed in x1 back. Handled as a WFI so the run
                          loop parks with the machine lock released. */
        static int off = -1;
        if (off < 0) off = getenv("HV_NO_PVLOCK") != NULL;
        if (off) return 0;
        cpu->x[0] = 0;
        if (cpu->x[1]) cpu->daif &= ~HV_PSTATE_I;
        else cpu->daif |= HV_PSTATE_I;
        return 2;
    default:
        return 0;
    }
}

static uint64_t psci(HvCpu *cpu, uint64_t fn) {
    static int log = -1;
    if (log < 0) log = getenv("HV_PSCI_LOG") != NULL;
    if (cpu->m->log >= 2) log = 1;   /* the machine's own log covers this too */
    if (log && ((uint32_t)fn >> 24 == 0x84 || (uint32_t)fn >> 24 == 0xc4))
        fprintf(stderr, "hengevm: psci %#llx x1 %#llx x2 %#llx x3 %#llx pc %llx ncpus %d\n", (unsigned long long)fn, (unsigned long long)cpu->x[1],
                (unsigned long long)cpu->x[2], (unsigned long long)cpu->x[3],
                (unsigned long long)cpu->pc, cpu->m->ncpus);
    switch ((uint32_t)fn) {
    case 0x84000000: return 0x10000;                         /* PSCI_VERSION: 1.0 */
    case 0x84000006: return 2;                               /* MIGRATE_INFO_TYPE: no trusted OS */
    case 0x84000008:                                         /* SYSTEM_OFF */
    case 0x84000009:                                         /* SYSTEM_RESET */
        if (cpu->m->log)
            fprintf(stderr, "hengevm: guest requested %s on cpu %u\n",
                    (uint32_t)fn == 0x84000008 ? "shutdown" : "restart", cpu->index);
        hv_machine_halt(cpu->m, (uint32_t)fn == 0x84000008 ? HV_HALT_SHUTDOWN : HV_HALT_RESET);
        return 0;
    case 0x8400000a: {                                       /* PSCI_FEATURES */
        uint32_t q = (uint32_t)cpu->x[1];
        if (q == 0x84000000 || q == 0x84000002 || q == 0x84000006 || q == 0x84000008 || q == 0x84000009 ||
            q == 0x8400000a || q == 0xc4000003 || q == 0x84000003 || q == 0xc4000004 || q == 0x84000004)
            return 0;
        return (uint64_t)-1;
    }
    case 0x84000002:                                         /* CPU_OFF: until another CPU_ON */
        cpu->off = 1;
        return 0;
    case 0x84000003: case 0xc4000003: {                      /* CPU_ON: target, entry point, context id */
        HvCpu *t = cpu_by_mpidr(cpu->m, cpu->x[1]);
        if (!t) return (uint64_t)-2;                         /* INVALID_PARAMETERS */
        if (!t->off) return (uint64_t)-4;                    /* ALREADY_ON */
        while (!__atomic_load_n(&t->parked_off, __ATOMIC_SEQ_CST)) {
            if (cpu->halted || t->halted) return (uint64_t)-3; /* DENIED: machine is stopping */
            sched_yield(); /* still on its way out of CPU_OFF */
        }
        uint64_t mpidr = t->mpidr, entry = cpu->x[2], context = cpu->x[3];
        if ((uint32_t)fn == 0x84000003) entry = (uint32_t)entry, context = (uint32_t)context;
        /* As at reset: EL1h, MMU and caches off, everything masked. Its thread
           waits on wake_mu, so it cannot see the CPU half reset. */
        pthread_mutex_lock(&t->pc_state->wake_mu);
        HvPerCpu *pcs = t->pc_state;
        if (cpu->halted || t->halted) {
            pthread_mutex_unlock(&pcs->wake_mu);
            return (uint64_t)-3;
        }
        hv_cpu_reset_index(t, cpu->m, t->index);
        t->mpidr = mpidr;
        t->pc = entry;
        t->x[0] = context;
        t->parked_off = 1;
        __atomic_store_n(&t->off, 0, __ATOMIC_RELEASE);
        pcs->wake_pending = 1;
        pthread_cond_signal(&pcs->wake_cv);
        pthread_mutex_unlock(&pcs->wake_mu);
        return 0;
    }
    case 0x84000004: case 0xc4000004: {                      /* AFFINITY_INFO: 0 on, 1 off */
        HvCpu *t = cpu_by_mpidr(cpu->m, cpu->x[1]);
        if (!t) return (uint64_t)-2;
        return t->off ? 1 : 0;
    }
    /* vmapple's paravirtualized PAC (XNU osfmk/arm64/hv_hvc.h): the guest
       cannot program PAC keys itself, so the hypervisor does. HengeVM never
       signs pointers, so there is nothing to program; each call only has to
       succeed (XNU spins on a nonzero x0). */
    case 0xc1000000: /* VMAPPLE_PAC_SET_INITIAL_STATE */
    case 0xc1000002: /* VMAPPLE_PAC_SET_A_KEYS */
    case 0xc1000003: /* VMAPPLE_PAC_SET_B_KEYS */
    case 0xc1000004: /* VMAPPLE_PAC_SET_EL0_DIVERSIFIER */
    case 0xc1000005: /* VMAPPLE_PAC_SET_EL0_DIVERSIFIER_AT_EL1 */
    case 0xc1000006: /* VMAPPLE_PAC_SET_G_KEY */
    case 0xc10000f0: /* VMAPPLE_PAC_NOP */
        return 0;
    case 0xc1000001: /* VMAPPLE_PAC_GET_DEFAULT_KEYS: x2 = default ROP id, x3 = default JOP id */
        cpu->x[2] = 0x524f50; /* the values only name keys, and no key is ever used */
        cpu->x[3] = 0x4a4f50;
        return 0;
    default:
        if (cpu->m->log) fprintf(stderr, "hengevm: unknown hypervisor call %#llx (x1 %#llx) at el%u\n",
                                 (unsigned long long)fn, (unsigned long long)cpu->x[1], cpu->el);
        return (uint64_t)-1;                                 /* NOT_SUPPORTED */
    }
}

/* Bring-up aid: x0-x3, and any string they point to (read with the current EL's mapping). */
void hv_debug_args(HvCpu *cpu) {
    for (int i = 0; i < 4; i++) fprintf(stderr, " x%d=%llx", i, (unsigned long long)cpu->x[i]);
    fprintf(stderr, "\n");
    for (int i = 0; i < 4; i++) {
        char str[161];
        int n = 0;
        for (; n < 160; n++) {
            uint64_t pa;
            if (hv_mmu_access(cpu, cpu->x[i] + n, HV_ACC_READ, cpu->el, &pa) || hv_phys_read(cpu->m, pa, &str[n], 1) ||
                str[n] < 9 || (unsigned char)str[n] > 126)
                break;
        }
        str[n] = 0;
        if (n >= 4) fprintf(stderr, "    x%d -> \"%s\"\n", i, str);
    }
}

int hv_exception_insn(HvCpu *cpu, uint32_t insn, uint64_t pc) {
    uint32_t imm16 = (insn >> 5) & 0xffff;
    uint32_t opc = (insn >> 21) & 7, ll = insn & 3;
    if (opc == 0 && ll == 1 && cpu->m->log >= 3) {
        fprintf(stderr, "svc #%x at %llx el%u:", imm16, (unsigned long long)pc, cpu->el);
        hv_debug_args(cpu);
    }
    if (opc == 0 && ll == 1) { /* SVC: ELR is the next instruction */
        /* Every syscall the guest makes, in order.  A trajectory of block addresses says where
           userland was but not what it asked for; the failures that matter ("launchd died") are
           visible only here, as one call whose result the caller never accepted.  Eight stores per
           syscall is nothing next to the trap itself. */
        {
            uint64_t slot = hv_svc_n++ & (HV_SVC_CAP - 1);
            hv_svc_pc[slot] = pc;
            hv_svc_imm[slot] = imm16;
            hv_svc_x16[slot] = cpu->x[16];
            hv_svc_x0[slot] = cpu->x[0];
            hv_svc_x1[slot] = cpu->x[1];
            hv_svc_x2[slot] = cpu->x[2];
            hv_svc_x3[slot] = cpu->x[3];
            hv_svc_ttbr0[slot] = cpu->ttbr0_el1;
            g_svc_m = cpu->m;
            hv_svc_el[slot] = cpu->el;
            hv_svc_ret[slot] = 0;
            hv_svc_path[slot][0] = 0;
            if (imm16 == 0x80 && (cpu->x[16] == 5 || cpu->x[16] == 59 || cpu->x[16] == 338)) {
                uint64_t va = cpu->x[0];
                int k = 0;
                for (; k < 95; k++) {
                    uint8_t ch = 0;
                    HvWalk w;
                    hv_mmu_walk(cpu, va + (uint64_t) k, HV_ACC_READ, (int) cpu->el, &w);
                    if (w.fsc || hv_phys_read(cpu->m, w.pa, &ch, 1) || ch == 0) break;
                    hv_svc_path[slot][k] = (char) ch;
                }
                hv_svc_path[slot][k] = 0;
            }
            hv_svc_last_slot[cpu->index] = slot;
            hv_svc_pending[cpu->index] = 1;
        }
        hv_take_exception(cpu, HV_EXC_SYNC, HV_EC_SVC64 << 26 | 1u << 25 | imm16, 0, 0, pc + 4);
        return 1;
    }
    if (opc == 0 && ll == 2 && cpu->el == 1) {
        hv_hvc_count++;
        hv_hvc_last_fn = (uint32_t) cpu->x[0];
        hv_hvc_last_pc = pc; /* HVC: the hypervisor's PSCI and vmapple's PAC calls */
        {
            /* Which hypercalls the kernel makes, in order: the PAC key setup is the first thing it
               does, so an hvc list that starts later means the entry never got that far. */
            static unsigned hvc_n;
            if (hvc_n < 32) {
                unsigned long long was = cpu->x[0];
                cpu->x[0] = (uint64_t) was;
                fprintf(stderr, "hengevm: hvc #%u fn %llx x1 %llx pc %llx el%u\n", hvc_n,
                        (unsigned long long) (uint32_t) was, (unsigned long long) cpu->x[1],
                        (unsigned long long) cpu->pc, cpu->el);
                hvc_n++;
            }
        }
        int hv = vmapple_hvc(cpu, (uint32_t) cpu->x[0]);
        if (hv == 2) {   /* the call parks this cpu: same exit as a WFI, and pc moves on */
            cpu->pc = pc + 4;
            cpu->exit_reason = HV_EXIT_WFI;
            return 1;
        }
        if (!hv) cpu->x[0] = psci(cpu, cpu->x[0]);
        if (!cpu->halted && !cpu->off) return 0; /* like a call: the block carries on (xnu makes one per context switch) */
        cpu->pc = pc + 4;
        cpu->exit_reason = cpu->halted ? HV_EXIT_HALT : HV_EXIT_BRANCH; /* CPU_OFF: the dispatcher parks it */
        return 1;
    }
    if (opc == 1 && ll == 0) { /* BRK */
        hv_take_exception(cpu, HV_EXC_SYNC, HV_EC_BRK << 26 | 1u << 25 | imm16, 0, 0, pc);
        return 1;
    }
    /* SMC with no EL3, HVC from EL0, HLT without halting debug, DCPS: undefined. */
    hv_raise_undef(cpu, pc);
    return 1;
}

int hv_check_interrupts(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    (void)m;
    if (p->fiq_line && !(cpu->daif & HV_PSTATE_F)) {
        hv_take_exception(cpu, HV_EXC_FIQ, 0, 0, 0, cpu->pc);
        return 1;
    }
    if (p->irq_line && !(cpu->daif & HV_PSTATE_I)) {
        hv_take_exception(cpu, HV_EXC_IRQ, 0, 0, 0, cpu->pc);
        return 1;
    }
    return 0;
}
