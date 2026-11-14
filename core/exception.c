#include "exception.h"

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

void hv_take_exception(HvCpu *cpu, int type, uint32_t esr, uint64_t far, int set_far, uint64_t ret) {
    {
        HvExcRec *r = &hv_exc_ring[hv_exc_at++ & (HV_EXC_RING - 1)];
        r->elr = ret, r->far = far, r->sp = cpu->sp, r->esr = esr;
        r->el = (uint32_t) cpu->el, r->type = (uint32_t) type;
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
    if (cpu->m->log >= 2 && (type != HV_EXC_SYNC || (esr >> 26) != 0x15) &&
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
        if (type == HV_EXC_SYNC && ((esr >> 26) & 0x3f) == 0x24) {
            HvWalk w;
            hv_mmu_walk(cpu, ret, HV_ACC_READ, 1, &w);
            uint32_t insn = 0;
            /* Always, whatever the walk said: the heavy diagnostics below sit behind conditions
               that a stale TLB can fail, and a fault that prints nothing is worse than one that
               prints only the essentials. */
            if (cpu->m->log >= 2)
                fprintf(stderr, "hengevm: data abort: esr %08x far %llx pc %llx el%u insn-ok %d "
                                "x0 %llx x2 %llx x4 %llx x16 %llx\n",
                        esr, (unsigned long long) far, (unsigned long long) ret, cpu->el,
                        w.fsc == 0, (unsigned long long) cpu->x[0], (unsigned long long) cpu->x[2],
                        (unsigned long long) cpu->x[4], (unsigned long long) cpu->x[16]);
            if (w.fsc == 0 && hv_phys_read(cpu->m, w.pa, &insn, 4) == 0) {
                snprintf(extra, sizeof extra, " insn %08x", insn);
                /*
                 * The registers at the fault, not at the panic. The guest runs its own handler
                 * before it reports, and that handler is free to use x0-x8: the dump in its panic
                 * report is not the faulting state, which is why it disagreed with the code that
                 * built the address.
                 */
                if (cpu->m->log >= 2) {
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
                {
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
                    uint64_t at = cpu->x[0] + 0x1C0, word = 0;
                    HvWalk ww;
                    hv_mmu_walk(cpu, at, HV_ACC_READ, 0, &ww);
                    if (ww.fsc == 0 && hv_phys_read(cpu->m, ww.pa, &word, 8) == 0)
                        fprintf(stderr, "hengevm: [x0+0x1c0] = %016llx (pa %llx)\n",
                                (unsigned long long) word, (unsigned long long) ww.pa);
                    else
                        fprintf(stderr, "hengevm: [x0+0x1c0] unreadable (fsc %u)\n", ww.fsc);
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
        if (type == HV_EXC_SYNC && ((esr >> 26) & 0x3f) == 0x25 && cpu->m->log >= 2) {
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
        fprintf(stderr, "exception type %d esr %08x far %llx from pc %llx el%u%s\n", type, esr,
                (unsigned long long)far, (unsigned long long)ret, cpu->el, extra);
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
static int vmapple_hvc(HvCpu *cpu, uint32_t fn) {
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
        uint64_t id = (uint32_t) cpu->x[1];
        cpu->x[0] = 0;
        if (id < (uint64_t) cpu->m->ncpus) {
            HvCpu *t = &cpu->m->cpus[id];
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
