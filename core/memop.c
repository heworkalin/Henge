#include "memop.h"

#include <stdlib.h>

#include <stdio.h>
#include <string.h>

#include "exception.h"
#include "machine.h"
#include "mmu.h"

static void ptw_note(HvCpu *cpu, uint64_t pa, uint64_t value, unsigned size, uint64_t pc, uint64_t va);

/* Decoding follows the "Loads and Stores" encoding index of the Arm ARM. */

static inline int64_t sext(uint64_t v, int bits) {
    return (int64_t)(v << (64 - bits)) >> (64 - bits);
}

static int decode_vector(uint32_t insn, HvMemOp *op) {
    uint32_t q = (insn >> 30) & 1, l = (insn >> 22) & 1, size = (insn >> 10) & 3;
    int single = (insn >> 24) & 1, post = (insn >> 23) & 1;
    op->kind = single ? HV_MEM_VEC_SINGLE : HV_MEM_VEC_MULTI;
    op->vec = 1;
    op->vq = q;
    op->load = l;
    op->store = !l;
    if (!post && op->rm != 0) return -1;
    if (single) {
        uint32_t opcode = (insn >> 13) & 7, s = (insn >> 12) & 1, r = (insn >> 21) & 1;
        uint32_t scale = opcode >> 1;
        op->selem = (((opcode & 1) << 1) | r) + 1;
        switch (scale) {
        case 3:
            if (!l || s) return -1;
            op->index = 0xff;
            op->size = size;
            break;
        case 0:
            op->index = q << 3 | s << 2 | size;
            op->size = 0;
            break;
        case 1:
            if (size & 1) return -1;
            op->index = q << 2 | s << 1 | size >> 1;
            op->size = 1;
            break;
        default:
            if (size & 2) return -1;
            if (!(size & 1)) {
                op->index = q << 1 | s;
                op->size = 2;
            } else {
                if (s) return -1;
                op->index = q;
                op->size = 3;
            }
            break;
        }
        op->bytes = op->selem << op->size;
    } else {
        if ((insn >> 21) & 1) return -1;
        static const uint8_t rpt[16] = {1, 0, 4, 0, 1, 0, 3, 1, 1, 0, 2, 0, 0, 0, 0, 0};
        static const uint8_t selem[16] = {4, 0, 1, 0, 3, 0, 1, 1, 2, 0, 1, 0, 0, 0, 0, 0};
        uint32_t opcode = (insn >> 12) & 0xf;
        if (!rpt[opcode]) return -1;
        if (size == 3 && !q && selem[opcode] > 1) return -1;
        op->rpt = rpt[opcode];
        op->selem = selem[opcode];
        op->size = size;
        op->bytes = (uint32_t)op->rpt * op->selem * (q ? 16 : 8);
    }
    if (post) {
        op->mode = HV_ADDR_POST;
        op->imm = op->bytes; /* used when Rm is 31; otherwise Xm is the amount */
    } else {
        op->mode = HV_ADDR_BASE;
        op->rm = 31;
    }
    return 0;
}

static int decode_exclusive(uint32_t insn, HvMemOp *op) {
    uint32_t size = insn >> 30, o2 = (insn >> 23) & 1, l = (insn >> 22) & 1, o1 = (insn >> 21) & 1;
    op->mode = HV_ADDR_BASE;
    op->align = 1;
    op->size = size;
    op->rm = 31;
    if (!o2 && !o1) {
        op->kind = HV_MEM_EXCL;
        op->load = l;
        op->store = !l;
        op->bytes = 1u << size;
        return 0;
    }
    if (!o2 && o1) {
        op->size = 2 + (size & 1);
        op->bytes = 2u << op->size;
        if (size >> 1) {
            op->kind = HV_MEM_EXCL;
            op->load = l;
            op->store = !l;
            op->rt2 = (insn >> 10) & 0x1f;
            return 0;
        }
        if ((op->rs & 1) || (op->rt & 1)) return -1;
        op->kind = HV_MEM_CASP;
        op->load = op->store = 1;
        return 0;
    }
    if (o2 && !o1) {
        op->kind = HV_MEM_ORDERED;
        op->load = l;
        op->store = !l;
        op->bytes = 1u << size;
        return 0;
    }
    op->kind = HV_MEM_CAS;
    op->load = op->store = 1;
    op->bytes = 1u << size;
    return 0;
}

int hv_mem_decode(uint32_t insn, HvMemOp *op) {
    memset(op, 0, sizeof *op);
    op->rt = insn & 0x1f;
    op->rn = (insn >> 5) & 0x1f;
    op->rt2 = (insn >> 10) & 0x1f;
    op->rm = op->rs = (insn >> 16) & 0x1f;
    uint32_t size = insn >> 30, v = (insn >> 26) & 1;

    if ((insn & 0x3b000000) == 0x18000000) { /* load register (literal) */
        op->kind = HV_MEM_LITERAL;
        op->mode = HV_ADDR_LITERAL;
        op->load = 1;
        op->vec = v;
        op->rm = 31;
        op->imm = sext((insn >> 5) & 0x7ffff, 19) * 4;
        if (v) {
            if (size == 3) return -1;
            op->size = 2 + size;
        } else {
            if (size == 3) return -1; /* PRFM is dropped before here */
            op->size = size == 1 ? 3 : 2;
            if (size == 2) op->sign = 64;
        }
        op->bytes = 1u << op->size;
        return 0;
    }
    if ((insn & 0xbe000000) == 0x0c000000) { /* SIMD structures */
        if (!v) return -1;
        return decode_vector(insn, op);
    }
    if ((insn & 0x3f000000) == 0x08000000) { /* exclusive, ordered, compare and swap */
        if (v) return -1;
        return decode_exclusive(insn, op);
    }
    if ((insn & 0x3f200c00) == 0x19000000) { /* LDAPUR/STLUR (RCpc, unscaled) */
        uint32_t opc = (insn >> 22) & 3;
        op->kind = HV_MEM_ORDERED;
        op->mode = HV_ADDR_OFFSET;
        op->imm = sext((insn >> 12) & 0x1ff, 9);
        op->size = size;
        op->rm = 31;
        op->bytes = 1u << size;
        op->store = opc == 0;
        op->load = opc != 0;
        if (opc == 2) {
            if (size == 3) return -1;
            op->sign = 64;
        } else if (opc == 3) {
            if (size >= 2) return -1;
            op->sign = 32;
        }
        return 0;
    }
    if ((insn & 0x38000000) == 0x28000000) { /* register pairs */
        uint32_t opc = insn >> 30, l = (insn >> 22) & 1, mode = (insn >> 23) & 3;
        op->kind = HV_MEM_PAIR;
        op->vec = v;
        op->load = l;
        op->store = !l;
        op->rm = 31;
        if (opc == 3) return -1;
        if (v) {
            op->size = 2 + opc;
        } else if (opc == 1) {
            if (!l || mode == 0) return -1; /* STGP (MTE) is not exposed */
            op->size = 2;
            op->sign = 64;
        } else {
            op->size = opc ? 3 : 2;
        }
        op->imm = sext((insn >> 15) & 0x7f, 7) << op->size;
        op->mode = mode == 1 ? HV_ADDR_POST : mode == 3 ? HV_ADDR_PRE : HV_ADDR_OFFSET;
        op->bytes = 2u << op->size;
        return 0;
    }
    if ((insn & 0x38000000) != 0x38000000) return -1;

    /* Single register: unsigned offset, imm9 forms, register offset, atomics, LDRA*. */
    uint32_t opc = (insn >> 22) & 3;
    op->kind = HV_MEM_SINGLE;
    op->vec = v;
    op->rm = 31;
    int atomic = !((insn >> 24) & 1) && ((insn >> 21) & 1) && ((insn >> 10) & 3) == 0;
    int ldra = !((insn >> 24) & 1) && ((insn >> 21) & 1) && ((insn >> 10) & 1);
    if (atomic) {
        if (v) return -1;
        uint32_t o3 = (insn >> 15) & 1, aop = (insn >> 12) & 7, a = (insn >> 23) & 1, r = (insn >> 22) & 1;
        op->mode = HV_ADDR_BASE;
        op->size = size;
        op->bytes = 1u << size;
        op->align = 1;
        if (o3 && aop == 4 && a && !r && op->rs == 31) { /* LDAPR */
            op->kind = HV_MEM_ORDERED;
            op->load = 1;
            return 0;
        }
        if (o3 && aop != 0) return -1; /* LD64B and friends */
        op->kind = HV_MEM_ATOMIC;
        op->atomic_op = o3 << 3 | aop;
        op->load = op->store = 1;
        return 0;
    }
    if (ldra) {
        if (v || size != 3) return -1;
        op->kind = HV_MEM_LDRA;
        op->load = 1;
        op->size = 3;
        op->bytes = 8;
        op->imm = sext(((insn >> 22) & 1) << 9 | ((insn >> 12) & 0x1ff), 10) * 8;
        op->mode = (insn >> 11) & 1 ? HV_ADDR_PRE : HV_ADDR_OFFSET;
        return 0;
    }
    if (v) {
        if (opc & 2) {
            if (size != 0) return -1;
            op->size = 4;
        } else {
            op->size = size;
        }
        op->load = opc & 1;
        op->store = !(opc & 1);
    } else {
        op->size = size;
        if (opc == 0) {
            op->store = 1;
        } else if (opc == 1) {
            op->load = 1;
        } else if (opc == 2) {
            if (size == 3) return -1; /* PRFM forms are dropped before here */
            op->load = 1;
            op->sign = 64;
        } else {
            if (size >= 2) return -1;
            op->load = 1;
            op->sign = 32;
        }
    }
    op->bytes = 1u << op->size;
    if ((insn >> 24) & 1) {
        op->mode = HV_ADDR_OFFSET;
        op->imm = (int64_t)((insn >> 10) & 0xfff) << op->size;
        return 0;
    }
    if ((insn >> 21) & 1) { /* register offset */
        uint32_t option = (insn >> 13) & 7;
        if (((insn >> 10) & 3) != 2 || !(option & 2)) return -1;
        op->mode = HV_ADDR_REG;
        op->rm = (insn >> 16) & 0x1f;
        op->ext = option;
        op->shift = (insn >> 12) & 1 ? op->size : 0;
        return 0;
    }
    op->imm = sext((insn >> 12) & 0x1ff, 9);
    switch ((insn >> 10) & 3) {
    case 0: op->mode = HV_ADDR_OFFSET; break;
    case 1: op->mode = HV_ADDR_POST; break;
    case 2:
        if (v) return -1;
        op->mode = HV_ADDR_OFFSET;
        op->unpriv = 1;
        break;
    default: op->mode = HV_ADDR_PRE; break;
    }
    return 0;
}

/* ---- C-side access ---- */

int hv_vmem_xfer(HvCpu *cpu, uint64_t va, void *buf, uint32_t len, int write, int el, uint64_t pc) {
    uint8_t *b = buf;
    uint64_t w = cpu->m->watch_va;
    /* Page granular, not address granular: the page is what the TLB check forces onto this path,
       and the store that matters may land anywhere in it. The physical form exists because the
       firmware runs with the MMU off, where a watched VA is the PA of whatever it is building. */
    int watched = write && w && ((w ^ va) & ~0xfffull) == 0;
    if (write && !watched && cpu->m->watch_pa) {
        /* Address granular here: the page is only what forces the slow path, and a whole page of
           zero fill (and then the image being copied in) would bury the one store worth seeing. */
        HvWalk ww;
        hv_mmu_walk(cpu, va, HV_ACC_WRITE, el, &ww);
        /* A window, not the exact address: the decoder that fills this region writes sixteen bytes
           at a time, so the store that puts the value in can start just past the address of
           interest and never cover it. Stores near it are what has to be read. */
        if ((ww.pa & ~0xfffull) == (cpu->m->watch_pa & ~0xfffull)
                && (va > cpu->m->watch_pa ? va - cpu->m->watch_pa : cpu->m->watch_pa - va) <= 0x40) {
            watched = 1;
            w = cpu->m->watch_pa;
        }
    }
    if (watched) {
        char val[32] = "-";
        if (w >= va && w - va < len) {
            uint64_t v = 0;
            uint32_t n = len - (uint32_t)(w - va);
            if (n > 8) n = 8;
            memcpy(&v, b + (w - va), n);
            snprintf(val, sizeof val, "%#llx", (unsigned long long) v);
        } else if (len) {
            /* A page watch fires for stores anywhere in the page: report what was written. */
            uint64_t v = 0;
            uint32_t n = len < 8 ? len : 8;
            memcpy(&v, b, n);
            snprintf(val, sizeof val, "head=%#llx", (unsigned long long) v);
        }
        fprintf(stderr, "hengevm: watch: %u-byte store at %llx (watched %s) by pc %llx x30 %llx\n",
                len, (unsigned long long) va, val, (unsigned long long) pc, (unsigned long long) cpu->x[30]);
        /* Also to a file: at the moment the kernel starts, stderr loses output. */
        {
            static int cap;
            const char *wd = getenv("HV_BLOCK_DUMP");
            if (wd && *wd && cap < 4096) {
                char p2[512];
                snprintf(p2, sizeof p2, "%s", wd);
                char *sl = strrchr(p2, '/');
                if (sl) {
                    snprintf(sl + 1, sizeof p2 - (size_t)(sl + 1 - p2), "watch.txt");
                    FILE *f = fopen(p2, "a");
                    if (f) {
                        if (cap == 0) {
                            /* Written by this file, which is recompiled whenever the watch code
                               changes: it is the one thing in the log that says which build is
                               actually running. */
                            fprintf(f, "# watch log from build %s %s\n", __DATE__, __TIME__);
                            fflush(f);
                        }
                        cap++;
                        fprintf(f, "store %u at %#llx (%s) = %s pc %#llx x30 %#llx\n",
                                len, (unsigned long long) va,
                                w == cpu->m->watch_va ? "va" : "pa", val, (unsigned long long) pc,
                                (unsigned long long) cpu->x[30]);
                        /* Every register, because the value came from one of them: the code that
                           writes it usually loaded it from a buffer several instructions earlier. */
                        fprintf(f, "  regs:");
                        for (int r = 0; r < 16; r++)
                            fprintf(f, " x%d=%llx", r, (unsigned long long) cpu->x[r]);
                        fprintf(f, "\n");
                        /* __DATA_CONST as the firmware leaves it, once per hit for the first few: the
                       image is copied down in pieces, so which moment counts is not knowable in
                       advance and comparing the copies says which one is complete. */
                    {
                        static int dc_hits;
                        if (w != cpu->m->watch_va && dc_hits < 8) {
                            dc_hits++;
                            size_t n = (size_t) 0x680000;
                            uint8_t *dc = malloc(n);
                            if (dc && hv_phys_read(cpu->m, 0x74504000ull, dc, (unsigned) n) == 0) {
                                char p9[512];
                                snprintf(p9, sizeof p9, "%s", wd);
                                char *sl9 = strrchr(p9, '/');
                                if (sl9) {
                                    snprintf(sl9 + 1, sizeof p9 - (size_t)(sl9 + 1 - p9),
                                             "dc-%02d.bin", dc_hits);
                                    FILE *df = fopen(p9, "wb");
                                    if (df) { fwrite(dc, 1, n, df); fclose(df); }
                                }
                            }
                            free(dc);
                        }
                    }
                    /* The kernel's __DATA_CONST as the firmware left it, once per run: comparing
                       two runs byte for byte says whether the wrong bytes are the same wrong bytes
                       every time (an emulation fault) or differ (a transfer fault). */
                    static int dc_done;
                    if (!dc_done && w == cpu->m->watch_va) {
                        dc_done = 1;
                        size_t n = (size_t) 0x680000;
                        uint8_t *dc = malloc(n);
                        if (dc && hv_phys_read(cpu->m, 0x74504000ull, dc, (unsigned) n) == 0) {
                            char p7[512];
                            snprintf(p7, sizeof p7, "%s", wd);
                            char *sl7 = strrchr(p7, '/');
                            if (sl7) {
                                snprintf(sl7 + 1, sizeof p7 - (size_t)(sl7 + 1 - p7), "dataconst-%s.bin",
                                         getenv("HV_DUMP_TAG") ? getenv("HV_DUMP_TAG") : "none");
                                FILE *df = fopen(p7, "wb");
                                if (df) { fwrite(dc, 1, n, df); fclose(df); }
                            }
                        }
                        free(dc);
                    }
                    /* The whole of guest RAM, once, when asked for: finding the compressed
                       payload wherever the firmware happens to have put it is the only way to
                       settle whether the bytes it decoded were the bytes that were sent. */
                    static int ram_done;
                    /* At the kernel's first write, not at the firmware's: the component buffers
                       are reused as each one arrives, and a dump taken earlier holds iBEC rather
                       than the kernel collection that was the last thing sent. */
                    if (!ram_done && getenv("HV_DUMP_RAM") && w == cpu->m->watch_va) {
                        ram_done = 1;
                        size_t n = (size_t) 512 << 20;
                        uint8_t *whole = malloc(n);
                        if (whole && hv_phys_read(cpu->m, 0x70000000ull, whole, (unsigned) n) == 0) {
                            char p8[512];
                            snprintf(p8, sizeof p8, "%s", wd);
                            char *sl8 = strrchr(p8, '/');
                            if (sl8) {
                                snprintf(sl8 + 1, sizeof p8 - (size_t)(sl8 + 1 - p8), "ram.bin");
                                FILE *rf = fopen(p8, "wb");
                                if (rf) { fwrite(whole, 1, n, rf); fclose(rf); }
                            }
                        }
                        free(whole);
                    }
                    /* The compressed stream the firmware is decoding, once per run: what the
                       guest actually received is the only way to tell a decoder that is wrong from
                       bytes that never arrived. The decoder's input pointer sits near 0x8EBF0000
                       and the payload is 21 MB, so this window covers it. */
                    static int recv_done;
                    if (!recv_done) {
                        recv_done = 1;
                        size_t n = (size_t) 32 << 20;
                        uint8_t *rec = malloc(n);
                        if (rec && hv_phys_read(cpu->m, 0x8E000000ull, rec, (unsigned) n) == 0) {
                            char p4[512];
                            snprintf(p4, sizeof p4, "%s", wd);
                            char *sl4 = strrchr(p4, '/');
                            if (sl4) {
                                snprintf(sl4 + 1, sizeof p4 - (size_t)(sl4 + 1 - p4), "recv.bin");
                                FILE *rf = fopen(p4, "wb");
                                if (rf) {
                                    fwrite(rec, 1, n, rf);
                                    fclose(rf);
                                    fprintf(f, "  recv window 0x8e000000..0x90000000 written\n");
                                }
                            }
                        }
                        free(rec);
                    }
                    /* The firmware's own code, around the instruction that did the store.
                           The pc is a virtual address — the firmware does run with its MMU on —
                           so it is translated first; reading a physical window at the pc itself
                           is what made earlier dumps disassemble to something else entirely. */
                        static int fw_dumps;
                        /* Only the firmware's own stores: the kernel hits number in the hundreds and
                           used up the window budget before the decoder ever ran. */
                        if (fw_dumps < 256 && w != cpu->m->watch_va) {
                            fw_dumps++;
                            HvWalk cw;
                            hv_mmu_walk(cpu, pc, HV_ACC_EXEC, el, &cw);
                            /* No silent fallback: a failed walk used to look exactly like a
                               successful one that translated to the same address, which is how a
                               window of zeroes was accepted as the executing code. */
                            uint64_t cpa = cw.fsc ? 0 : cw.pa;
                            if (!cpa) {
                                HvWalk rw2;
                                hv_mmu_walk(cpu, pc, HV_ACC_READ, el, &rw2);
                                if (!rw2.fsc) cpa = rw2.pa;
                            }
                            uint32_t words[4] = {0, 0, 0, 0};
                            if (cpa) hv_phys_read(cpu->m, cpa & ~3ull, words, sizeof words);
                            fprintf(f, "  code pc %#llx -> pa %#llx (fsc %#x)  insn %08x %08x %08x %08x\n",
                                    (unsigned long long) pc, (unsigned long long) cpa, cw.fsc,
                                    words[0], words[1], words[2], words[3]);
                            /* Once, the whole firmware text: 64 KB windows around a hit are
                               enough to see the loop but not the function that set it up. */
                            /* Refreshed on every firmware hit, not once: the range is reused by
                               each stage, and the code that matters is the last one loaded there. */
                            static int ibec_dumps;
                            if (ibec_dumps < 200) {
                                ibec_dumps++;
                                uint8_t *txt = malloc(4u << 20);
                                if (txt && hv_phys_read(cpu->m, 0x70000000ull, txt, 4u << 20) == 0) {
                                    char p5[512];
                                    snprintf(p5, sizeof p5, "%s", wd);
                                    char *sl5 = strrchr(p5, '/');
                                    if (sl5) {
                                        snprintf(sl5 + 1, sizeof p5 - (size_t)(sl5 + 1 - p5), "ibec.bin");
                                        FILE *tf = fopen(p5, "wb");
                                        if (tf) {
                                            fwrite(txt, 1, 4u << 20, tf);
                                            fclose(tf);
                                            fprintf(f, "  ibec text pa 0x70000000..0x70400000 written\n");
                                        }
                                    }
                                }
                                free(txt);
                            }
                            /* What the registers point at, since guessing where the decoder's
                               input lives has been wrong twice. Only the pointers that fall in
                               guest RAM, and only for the first few hits. */
                            static int reg_dumps;
                            if (reg_dumps < 8) {
                                reg_dumps++;
                                for (int r = 0; r < 16; r++) {
                                    uint64_t v = cpu->x[r];
                                    if (v < 0x70000000ull || v >= 0x90000000ull) continue;
                                    uint64_t page = v & ~0xfffull;
                                    uint8_t *buf = malloc(4096);
                                    if (!buf || hv_phys_read(cpu->m, page, buf, 4096) != 0) { free(buf); continue; }
                                    char p6[512];
                                    snprintf(p6, sizeof p6, "%s", wd);
                                    char *sl6 = strrchr(p6, '/');
                                    if (sl6) {
                                        snprintf(sl6 + 1, sizeof p6 - (size_t)(sl6 + 1 - p6),
                                                 "hit%02d-x%d.bin", reg_dumps, r);
                                        FILE *gf = fopen(p6, "wb");
                                        if (gf) { fwrite(buf, 1, 4096, gf); fclose(gf); }
                                    }
                                    free(buf);
                                }
                            }
                            uint8_t *code = malloc(1u << 16);
                            uint64_t win = cpa & ~0xffffull;
                            if (code && hv_phys_read(cpu->m, win, code, 1u << 16) == 0) {
                                char p3[512];
                                snprintf(p3, sizeof p3, "%s", wd);
                                char *sl3 = strrchr(p3, '/');
                                if (sl3) {
                                    snprintf(sl3 + 1, sizeof p3 - (size_t)(sl3 + 1 - p3),
                                             "fwcode-%02d.bin", fw_dumps);
                                    FILE *cf = fopen(p3, "wb");
                                    if (cf) {
                                        fwrite(code, 1, 1u << 16, cf);
                                        fclose(cf);
                                        fprintf(f, "  code window pa %#llx..%#llx written\n",
                                                (unsigned long long) win,
                                                (unsigned long long) (win + (1u << 16)));
                                    }
                                }
                            }
                            free(code);
                        }
                        fflush(f);
                        fclose(f);
                    }
                }
            }
        }
    }
    while (len) {
        uint32_t chunk = 4096 - (uint32_t)(va & 4095);
        if (chunk > len) chunk = len;
        uint64_t pa;
        int had_entry = 0;
        if (cpu->m->stats_interval) { /* --stats: was the TLB entry there before this access? */
            HvTlbEntry *t = hv_tlb_ent(cpu, (va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), hv_tlb_table(cpu, el));
            uint64_t tag = write ? t->write_tag : t->read_tag;
            had_entry = tag != HV_TLB_INVALID && ((va - tag) >> HV_TLB_PAGE_BITS) == 0 ? 1 :
                        tag != HV_TLB_INVALID && ((va - tag - HV_TLB_ADJ) >> HV_TLB_PAGE_BITS) == 0 ? 2 : 0;
            if (!had_entry) {
                int other = t->read_tag != HV_TLB_INVALID || t->write_tag != HV_TLB_INVALID || t->exec_tag != HV_TLB_INVALID;
                cpu->m->stats.tlb_miss_kind[(el ? 2 : 0) + other]++;
            }
        }
        uint32_t fsc = hv_mmu_access(cpu, va, write ? HV_ACC_WRITE : HV_ACC_READ, el, &pa);
        if (fsc) {
            hv_raise_abort(cpu, 0, va, fsc, write, fsc >= 0x14 && fsc <= 0x17, pc);
            return 1;
        }
        int rc = 0;
        if (cpu->m->stats_interval) {
            HvRam *ram = hv_ram_lookup(cpu->m, pa);
            uint64_t inpage = va & ((1ull << HV_TLB_PAGE_BITS) - 1);
            int why = !ram ? 0 : write && ram->code_pages[(pa - ram->pa) >> HV_TLB_PAGE_BITS] ? 1
                    : inpage + len > (1ull << HV_TLB_PAGE_BITS) - HV_TLB_ADJ ? 2 : had_entry == 0 ? 3 : had_entry == 2 ? 5 : 4;
            cpu->m->stats.slow_why[why]++;
            if (why == 3) { /* a miss the light refill could not fill: a small guest page? */
                HvWalk w;
                hv_mmu_walk(cpu, va, write ? HV_ACC_WRITE : HV_ACC_READ, el, &w);
                if (!w.fsc && w.block_size < (1ull << HV_TLB_PAGE_BITS)) {
                    cpu->m->stats.small_pages++;
                    uint64_t key = (va >> 20) << 1 | (el ? 1 : 0);
                    for (int i = 0; i < 8; i++) {
                        if (cpu->m->stats.small_at[i].mb == key) { cpu->m->stats.small_at[i].n++; break; }
                        if (!cpu->m->stats.small_at[i].mb) { cpu->m->stats.small_at[i].mb = key; cpu->m->stats.small_at[i].n = 1; break; }
                    }
                }
            }
        }
        /* Watch one page while diagnosing: a kernel global that holds a pointer somebody wrote a
           bad value into. The store that does it names itself, with what it wrote. */
        if (cpu->m->log >= 2 && write && va >= 0xfffffe001f1f8000ull && va < 0xfffffe001f1f9000ull)
            fprintf(stderr, "hengevm: watch: write %u bytes at %llx (pa %llx) value %llx pc %llx\n",
                    chunk, (unsigned long long) va, (unsigned long long) pa,
                    (unsigned long long) b[0], (unsigned long long) pc);
        if (write) {
            uint64_t v = 0;
            for (uint32_t x = 0; x < chunk && x < 8; x++) v |= (uint64_t) b[x] << (8 * x);
            ptw_note(cpu, pa, v, chunk, pc, va);
        }
        if (hv_phys_ptr(cpu->m, pa, chunk)) {
            rc = write ? hv_phys_write(cpu->m, pa, b, chunk) : hv_phys_read(cpu->m, pa, b, chunk);
        } else {
            for (uint32_t off = 0; off < chunk && !rc; off += 8) { /* MMIO: at most 8 bytes at a time */
                uint32_t piece = chunk - off < 8 ? chunk - off : 8;
                rc = write ? hv_phys_write(cpu->m, pa + off, b + off, piece) : hv_phys_read(cpu->m, pa + off, b + off, piece);
            }
        }
        if (rc) {
            if (cpu->m->log)
                fprintf(stderr, "unmapped %s: pa %llx (va %llx) size %u pc %llx\n", write ? "write" : "read",
                        (unsigned long long)pa, (unsigned long long)va, chunk, (unsigned long long)pc);
            hv_raise_abort(cpu, 0, va, HV_FSC_EXTERNAL, write, 0, pc);
            return 1;
        }
        va += chunk;
        b += chunk;
        len -= chunk;
    }
    return 0;
}

static uint64_t gpr(HvCpu *cpu, int r) {
    return r == 31 ? 0 : cpu->x[r];
}

/* Whether a value looks like an address in this process's own libraries rather than anything a
   guest could have: the guest's addresses are its own small or kernel ones, and the libraries sit
   within a few hundred MB of this function. */
int hv_looks_host(uint64_t v) {
    uintptr_t ref = (uintptr_t) &hv_looks_host;
    uint64_t d = v > ref ? v - (uintptr_t) ref : (uintptr_t) ref - v;
    return v != 0 && d < (1ull << 28);
}

static void set_gpr(HvCpu *cpu, int r, uint64_t v) {
    /* A load that brings one of our own addresses into a guest register: the guest cannot have
       produced it, so whatever answered the read returned a host pointer. */
    if (cpu->m->log >= 2 && r != 31 && hv_looks_host(v))
        fprintf(stderr, "hengevm: load into x%d gave a host address %llx (pc %llx)\n",
                r, (unsigned long long) v, (unsigned long long) cpu->pc);
    if (r != 31) cpu->x[r] = v;
}

static uint64_t base_reg(HvCpu *cpu, int r) {
    return r == 31 ? cpu->sp : cpu->x[r];
}

static void set_base(HvCpu *cpu, int r, uint64_t v) {
    if (r == 31) cpu->sp = v;
    else cpu->x[r] = v;
}

static uint8_t *vreg(HvCpu *cpu, int r) {
    return (uint8_t *)&cpu->v[r & 31];
}

static uint64_t extend(uint64_t v, const HvMemOp *op) {
    int bits = 8 << op->size;
    if (op->sign == 64) return (uint64_t)sext(v, bits);
    if (op->sign == 32) return (uint32_t)sext(v, bits);
    return bits == 64 ? v : v & ((1ull << bits) - 1);
}

static uint64_t le(const uint8_t *p, int bytes) {
    uint64_t v = 0;
    memcpy(&v, p, bytes);
    return v;
}

static int alignment_fault(HvCpu *cpu, uint64_t va, int write, uint64_t pc) {
    hv_raise_abort(cpu, 0, va, HV_FSC_ALIGNMENT, write, 0, pc);
    return 1;
}

/* For atomics: a host pointer when va is RAM the guest may write, else NULL with
   *mmio_pa set. Returns -1 after an abort. */
static int rmw_target(HvCpu *cpu, uint64_t va, uint32_t bytes, int el, uint64_t pc, void **host, uint64_t *mmio_pa) {
    uint64_t pa;
    uint32_t fsc = hv_mmu_access(cpu, va, HV_ACC_WRITE, el, &pa);
    if (fsc) {
        hv_raise_abort(cpu, 0, va, fsc, 1, fsc >= 0x14 && fsc <= 0x17, pc);
        return -1;
    }
    HvRam *ram = hv_ram_lookup(cpu->m, pa);
    if (ram && ram->readonly) { /* ROM: nothing to write; treat like a device */
        *host = NULL;
        *mmio_pa = pa;
        return 0;
    }
    *host = hv_phys_ptr(cpu->m, pa, bytes);
    if (*host) {
        /* Same rule as other writes: drop translations made from this page. */
        uint64_t pg = (pa - ram->pa) >> HV_TLB_PAGE_BITS;
        if (ram->code_pages[pg]) hv_invalidate_code(cpu->m, pa);
    }
    *mmio_pa = pa;
    return 0;
}

static int cas_host(void *p, int size, uint64_t *expected, uint64_t desired) {
    switch (size) {
    case 0: { uint8_t e = (uint8_t)*expected; int ok = __atomic_compare_exchange_n((uint8_t *)p, &e, (uint8_t)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; return ok; }
    case 1: { uint16_t e = (uint16_t)*expected; int ok = __atomic_compare_exchange_n((uint16_t *)p, &e, (uint16_t)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; return ok; }
    case 2: { uint32_t e = (uint32_t)*expected; int ok = __atomic_compare_exchange_n((uint32_t *)p, &e, (uint32_t)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; return ok; }
    default: return __atomic_compare_exchange_n((uint64_t *)p, expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
}

static uint64_t load_host(void *p, int size) {
    switch (size) {
    case 0: return __atomic_load_n((uint8_t *)p, __ATOMIC_SEQ_CST);
    case 1: return __atomic_load_n((uint16_t *)p, __ATOMIC_SEQ_CST);
    case 2: return __atomic_load_n((uint32_t *)p, __ATOMIC_SEQ_CST);
    default: return __atomic_load_n((uint64_t *)p, __ATOMIC_SEQ_CST);
    }
}

static uint64_t atomic_result(int aop, int size, uint64_t old, uint64_t s) {
    int bits = 8 << size;
    int64_t so = sext(old, bits), ss = sext(s, bits);
    uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
    old &= mask;
    s &= mask;
    switch (aop) {
    case 0: return (old + s) & mask;             /* LDADD */
    case 1: return old & ~s;                     /* LDCLR */
    case 2: return old ^ s;                      /* LDEOR */
    case 3: return old | s;                      /* LDSET */
    case 4: return (so > ss ? old : s);          /* LDSMAX */
    case 5: return (so < ss ? old : s);          /* LDSMIN */
    case 6: return old > s ? old : s;            /* LDUMAX */
    case 7: return old < s ? old : s;            /* LDUMIN */
    default: return s;                           /* SWP */
    }
}


/* The same watch, for the paths that do not go through hv_vmem_xfer: an atomic or exclusive
   store to the watched page would otherwise never be seen, and the one write that matters may
   well be one of those. */
static void watch_store(HvCpu *cpu, uint64_t va, uint64_t v, unsigned len, uint64_t pc, const char *how) {
    uint64_t w = cpu->m->watch_va;
    if (!w || ((w ^ va) & ~0xfffull) != 0) return;
    const char *wd = getenv("HV_BLOCK_DUMP");
    if (!wd || !*wd) return;
    char p2[512];
    snprintf(p2, sizeof p2, "%s", wd);
    char *sl = strrchr(p2, '/');
    if (!sl) return;
    snprintf(sl + 1, sizeof p2 - (size_t)(sl + 1 - p2), "watch.txt");
    FILE *f = fopen(p2, "a");
    if (!f) return;
    fprintf(f, "%s %u at %#llx = %#llx (watched %#llx) pc %#llx x30 %#llx x0 %#llx x1 %#llx x2 %#llx\n",
            how, len, (unsigned long long) va, (unsigned long long) v, (unsigned long long) w,
            (unsigned long long) pc, (unsigned long long) cpu->x[30], (unsigned long long) cpu->x[0],
            (unsigned long long) cpu->x[1], (unsigned long long) cpu->x[2]);
    fclose(f);
}

/* Log a store that lands in a page the walk uses as a translation table. Page table entries are
   installed through several paths (a plain store, an atomic exchange, a compare-and-swap, an
   exclusive pair), and only logging all of them can say whether a missing entry was never written,
   written and undone, or written somewhere else. */
static void ptw_note(HvCpu *cpu, uint64_t pa, uint64_t value, unsigned size, uint64_t pc, uint64_t va) {
    if (!cpu->m->ptw_log) return;
    uint64_t page = pa & ~0xfffull;
    for (uint32_t k = 0; k < cpu->m->ptw_n && k < 512; k++) {
        if (cpu->m->ptw_pages[k] != page) continue;
        fprintf(stderr, "hengevm: ptw %llx [%llu] = %016llx size %u pc %llx (va %llx)\n",
                (unsigned long long) pa, (unsigned long long) ((pa - page) / 8),
                (unsigned long long) value, size, (unsigned long long) pc, (unsigned long long) va);
        return;
    }
}

static int do_atomic(HvCpu *cpu, const HvMemOp *op, uint64_t va, int el, uint64_t pc) {
    int size = op->size;
    uint32_t bytes = 1u << size;
    if (va & (bytes - 1)) return alignment_fault(cpu, va, 1, pc);
    void *host;
    uint64_t pa;
    if (rmw_target(cpu, va, bytes, el, pc, &host, &pa)) return 1;
    uint64_t mask = size == 3 ? ~0ull : (1ull << (8 << size)) - 1;
    uint64_t old, s = gpr(cpu, op->rs);
    if (op->kind == HV_MEM_CAS) {
        uint64_t desired = gpr(cpu, op->rt) & mask;
        watch_store(cpu, va, desired, bytes, pc, "cas");
        if (host) {
            old = s & mask;
            cas_host(host, size, &old, desired);
        } else {
            old = 0;
            if (hv_phys_read(cpu->m, pa, &old, bytes)) goto external;
            if (old == (s & mask)) ptw_note(cpu, pa, desired, bytes, pc, va);
            if (old == (s & mask) && hv_phys_write(cpu->m, pa, &desired, bytes)) goto external;
        }
        set_gpr(cpu, op->rs, old);
        return 0;
    }
    watch_store(cpu, va, s, bytes, pc, "atomic-op");
    if (host) {
        old = load_host(host, size);
        while (!cas_host(host, size, &old, atomic_result(op->atomic_op, size, old, s))) {
        }
    } else {
        old = 0;
        if (hv_phys_read(cpu->m, pa, &old, bytes)) goto external;
        uint64_t n = atomic_result(op->atomic_op, size, old, s);
        ptw_note(cpu, pa, n, bytes, pc, va);
        if (hv_phys_write(cpu->m, pa, &n, bytes)) goto external;
    }
    set_gpr(cpu, op->rt, old & mask);
    return 0;
external:
    hv_raise_abort(cpu, 0, va, HV_FSC_EXTERNAL, 1, 0, pc);
    return 1;
}

static int do_casp(HvCpu *cpu, const HvMemOp *op, uint64_t va, int el, uint64_t pc) {
    uint32_t esize = 1u << op->size, bytes = esize * 2;
    if (va & (bytes - 1)) return alignment_fault(cpu, va, 1, pc);
    void *host;
    uint64_t pa;
    if (rmw_target(cpu, va, bytes, el, pc, &host, &pa)) return 1;
    uint64_t mask = esize == 8 ? ~0ull : 0xffffffffull;
    uint64_t c0 = gpr(cpu, op->rs) & mask, c1 = gpr(cpu, op->rs + 1) & mask;
    uint64_t n0 = gpr(cpu, op->rt) & mask, n1 = gpr(cpu, op->rt + 1) & mask;
    uint64_t o0, o1;
    if (host && esize == 4) {
        uint64_t old = c0 | c1 << 32;
        cas_host(host, 3, &old, n0 | n1 << 32);
        o0 = old & mask;
        o1 = old >> 32;
    } else if (host) {
        __uint128_t old = (__uint128_t)c1 << 64 | c0;
        __atomic_compare_exchange_n((__uint128_t *)host, &old, (__uint128_t)n1 << 64 | n0, 0, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST);
        o0 = (uint64_t)old;
        o1 = (uint64_t)(old >> 64);
    } else {
        uint8_t buf[16] = {0};
        if (hv_phys_read(cpu->m, pa, buf, esize) || hv_phys_read(cpu->m, pa + esize, buf + esize, esize)) goto external;
        o0 = le(buf, esize);
        o1 = le(buf + esize, esize);
        if (o0 == c0 && o1 == c1) {
            ptw_note(cpu, pa, n0, esize, pc, va);
            ptw_note(cpu, pa + esize, n1, esize, pc, va);
            if (hv_phys_write(cpu->m, pa, &n0, esize) || hv_phys_write(cpu->m, pa + esize, &n1, esize)) goto external;
        }
    }
    set_gpr(cpu, op->rs, o0);
    set_gpr(cpu, op->rs + 1, o1);
    return 0;
external:
    hv_raise_abort(cpu, 0, va, HV_FSC_EXTERNAL, 1, 0, pc);
    return 1;
}

static int do_exclusive(HvCpu *cpu, const HvMemOp *op, uint64_t va, int el, uint64_t pc) {
    uint32_t bytes = op->bytes;
    int pair = bytes == (2u << op->size); /* LDXP/STXP: decode sets bytes to two registers' worth */
    if (va & (bytes - 1)) return alignment_fault(cpu, va, op->store, pc);
    uint32_t esize = pair ? 1u << op->size : bytes;
    if (op->load) {
        uint8_t buf[16];
        if (hv_vmem_xfer(cpu, va, buf, bytes, 0, el, pc)) return 1;
        cpu->excl_addr = va;
        cpu->excl_size = bytes;
        cpu->excl_val[0] = le(buf, esize);
        cpu->excl_val[1] = pair ? le(buf + esize, esize) : 0;
        set_gpr(cpu, op->rt, cpu->excl_val[0]);
        if (pair) set_gpr(cpu, op->rt2, cpu->excl_val[1]);
        return 0;
    }
    void *host;
    uint64_t pa;
    if (rmw_target(cpu, va, bytes, el, pc, &host, &pa)) return 1;
    uint64_t mask = esize == 8 ? ~0ull : (1ull << (esize * 8)) - 1;
    uint64_t n0 = gpr(cpu, op->rt) & mask, n1 = pair ? gpr(cpu, op->rt2) & mask : 0;
    int ok = 0;
    if (cpu->excl_addr == va && cpu->excl_size == bytes) watch_store(cpu, va, n0, esize, pc, "exclusive");
    if (cpu->excl_addr == va && cpu->excl_size == bytes) {
        if (!host) {
            ptw_note(cpu, pa, n0, esize, pc, va);
            if (pair) ptw_note(cpu, pa + esize, n1, esize, pc, va);
            ok = !hv_phys_write(cpu->m, pa, &n0, esize) && (!pair || !hv_phys_write(cpu->m, pa + esize, &n1, esize));
        } else if (!pair) {
            uint64_t expect = cpu->excl_val[0];
            ok = cas_host(host, op->size, &expect, n0);
        } else if (esize == 4) {
            uint64_t expect = cpu->excl_val[0] | cpu->excl_val[1] << 32;
            ok = cas_host(host, 3, &expect, n0 | n1 << 32);
        } else {
            __uint128_t expect = (__uint128_t)cpu->excl_val[1] << 64 | cpu->excl_val[0];
            ok = __atomic_compare_exchange_n((__uint128_t *)host, &expect, (__uint128_t)n1 << 64 | n0, 0,
                                             __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        }
    }
    cpu->excl_addr = ~0ull;
    set_gpr(cpu, op->rs, ok ? 0 : 1);
    return 0;
}

static int do_vector(HvCpu *cpu, const HvMemOp *op, uint64_t va, int el, uint64_t pc) {
    if (op->store) watch_store(cpu, va, 0, op->bytes, pc, "vector-store");
    uint8_t buf[64];
    uint32_t ebytes = 1u << op->size;
    if (op->store) {
        uint32_t off = 0;
        if (op->kind == HV_MEM_VEC_MULTI) {
            uint32_t elements = (op->vq ? 16 : 8) / ebytes;
            for (int r = 0; r < op->rpt; r++)
                for (uint32_t e = 0; e < elements; e++)
                    for (int s = 0; s < op->selem; s++, off += ebytes)
                        memcpy(buf + off, vreg(cpu, op->rt + r + s) + e * ebytes, ebytes);
        } else {
            for (int s = 0; s < op->selem; s++, off += ebytes)
                memcpy(buf + off, vreg(cpu, op->rt + s) + op->index * ebytes, ebytes);
        }
        return hv_vmem_xfer(cpu, va, buf, op->bytes, 1, el, pc);
    }
    if (hv_vmem_xfer(cpu, va, buf, op->bytes, 0, el, pc)) return 1;
    uint32_t off = 0;
    if (op->kind == HV_MEM_VEC_MULTI) {
        uint32_t elements = (op->vq ? 16 : 8) / ebytes;
        for (int r = 0; r < op->rpt; r++)
            for (uint32_t e = 0; e < elements; e++)
                for (int s = 0; s < op->selem; s++, off += ebytes)
                    memcpy(vreg(cpu, op->rt + r + s) + e * ebytes, buf + off, ebytes);
        if (!op->vq) /* 64-bit destinations clear the top half */
            for (int r = 0; r < op->rpt; r++)
                for (int s = 0; s < op->selem; s++) memset(vreg(cpu, op->rt + r + s) + 8, 0, 8);
    } else if (op->index == 0xff) {
        for (int s = 0; s < op->selem; s++, off += ebytes) {
            uint8_t *d = vreg(cpu, op->rt + s);
            memset(d, 0, 16);
            for (uint32_t i = 0; i < (op->vq ? 16u : 8u); i += ebytes) memcpy(d + i, buf + off, ebytes);
        }
    } else {
        for (int s = 0; s < op->selem; s++, off += ebytes)
            memcpy(vreg(cpu, op->rt + s) + op->index * ebytes, buf + off, ebytes);
    }
    return 0;
}

/* HV_PREDSTAT (measuring, with every access sent here): how often an access is on the same page as the last one
   through the same base register, or the last one by the same instruction, or the access just before. The page is
   keyed with the address space for user addresses (a process switch is a miss, as it would be for a prediction). */
static void pred_stat(HvCpu *cpu, const HvMemOp *op, uint64_t va, uint64_t pc, int el) {
    static __thread uint64_t last_reg[2][32], last_site[1 << 16], last_any;
    static __thread uint64_t n, reg_hit, site_hit, either, any_hit, sp_n, sp_hit, k_n, k_reg, k_site;
    int kernel = va >> 63;
    uint64_t key = va >> 14;
    if (!kernel) key ^= (cpu->ttbr0_el1 & ~1ull) << 20; /* user pages of different address spaces differ */
    int r = op->mode == HV_ADDR_LITERAL ? 31 : op->rn; /* 31 here is SP (or the literal's PC) */
    uint32_t s = (uint32_t)((pc >> 2) * 2654435761u) >> 16;
    int rh = last_reg[el ? 1 : 0][r] == key, sh = last_site[s] == key;
    n++, reg_hit += rh, site_hit += sh, either += rh || sh, any_hit += last_any == key;
    if (r == 31 && op->mode != HV_ADDR_LITERAL) sp_n++, sp_hit += rh;
    if (kernel) k_n++, k_reg += rh, k_site += sh;
    last_reg[el ? 1 : 0][r] = key, last_site[s] = key, last_any = key;
    if ((n & ((1u << 24) - 1)) == 0) {
        fprintf(stderr, "predstat cpu %u: %lluM accesses: same page as the base register's last %.1f%%, as the instruction's last %.1f%%, "
                        "either %.1f%%, as the access before %.1f%%; SP-based %.1f%% of accesses (register hit %.1f%%); kernel %.1f%% "
                        "(register %.1f%%, instruction %.1f%%)\n",
                cpu->index, (unsigned long long)(n >> 20), 100.0 * reg_hit / n, 100.0 * site_hit / n, 100.0 * either / n,
                100.0 * any_hit / n, 100.0 * sp_n / n, sp_n ? 100.0 * sp_hit / sp_n : 0, 100.0 * k_n / n,
                k_n ? 100.0 * k_reg / k_n : 0, k_n ? 100.0 * k_site / k_n : 0);
        n = reg_hit = site_hit = either = any_hit = sp_n = sp_hit = k_n = k_reg = k_site = 0;
    }
}

/* Page table pages, sampled and diffed: a slot that changes is a page table entry being
   written, and the order of changes is the order the kernel installed them. Sampling costs a
   few pages copied per interval and catches writes from every path, including the ones the
   JIT emits directly into a translated block. */
void hv_ptw_sample(HvMachine *m) {
    if (!m->ptw_log || !m->ptw_n) return;
    size_t page = 0x1000, total = (size_t) m->ptw_n * page;
    if (!m->ptw_shadow) {
        m->ptw_shadow = calloc(1, 512 * page);
        if (!m->ptw_shadow) return;
        total = 0; /* first sample: record everything, so the baseline is the real contents */
    }
    for (uint32_t k = 0; k < m->ptw_n && k < 512; k++) {
        uint8_t now[0x1000];
        if (hv_phys_read(m, m->ptw_pages[k], now, sizeof now) != 0) continue;
        uint8_t *was = m->ptw_shadow + (size_t) k * page;
        for (size_t off = 0; off < page; off += 8) {
            uint64_t a, b;
            memcpy(&a, was + off, 8);
            memcpy(&b, now + off, 8);
            if (a == b) continue;
            fprintf(stderr, "hengevm: ptw %llx [%zu] %016llx -> %016llx (blocks %llu)\n",
                    (unsigned long long) m->ptw_pages[k], off / 8,
                    (unsigned long long) a, (unsigned long long) b,
                    (unsigned long long) m->stats.blocks);
            memcpy(was + off, &b, 8);
        }
    }
    (void) total;
}

int hv_mem_emulate(HvCpu *cpu, uint32_t insn, uint64_t pc) {
    cpu->m->access_pc = pc;
    {   /* one-shot: is the slow path in use at all during the kernel's init? */
        static int seen;
        if (!seen && cpu->m->log >= 2 && pc >= 0xfffffe0000000000ull) {
            seen = 1;
            fprintf(stderr, "hengevm: memop first kernel access: slow_mem %d pc %llx insn %08x\n",
                    cpu->m->slow_mem, (unsigned long long) pc, insn);
        }
    }
    HvMemOp op;
    if (hv_mem_decode(insn, &op) != 0) {
        hv_raise_undef(cpu, pc);
        return 1;
    }
    int el = op.unpriv ? 0 : (int)cpu->el;
    uint64_t base = op.mode == HV_ADDR_LITERAL ? pc : base_reg(cpu, op.rn);
    uint64_t va = base;
    switch (op.mode) {
    case HV_ADDR_OFFSET:
    case HV_ADDR_PRE:
    case HV_ADDR_LITERAL:
        va = base + op.imm;
        break;
    case HV_ADDR_REG: {
        uint64_t m = gpr(cpu, op.rm);
        switch (op.ext) {
        case 2: m = (uint32_t)m; break;
        case 6: m = (uint64_t)(int64_t)(int32_t)m; break;
        default: break;
        }
        va = base + (m << op.shift);
        break;
    }
    default:
        break;
    }
    if (hv_knob[HV_K_PREDSTAT]) pred_stat(cpu, &op, va, pc, el);

    int rc = 0;
    switch (op.kind) {
    case HV_MEM_EXCL:
        rc = do_exclusive(cpu, &op, va, el, pc);
        break;
    case HV_MEM_CAS:
    case HV_MEM_ATOMIC:
        rc = do_atomic(cpu, &op, va, el, pc);
        break;
    case HV_MEM_CASP:
        rc = do_casp(cpu, &op, va, el, pc);
        break;
    case HV_MEM_VEC_MULTI:
    case HV_MEM_VEC_SINGLE:
        rc = do_vector(cpu, &op, va, el, pc);
        break;
    default: {
        if (op.align && (va & (op.bytes - 1))) return alignment_fault(cpu, va, op.store, pc);
        int pair = op.kind == HV_MEM_PAIR;
        uint32_t esize = 1u << op.size;
        uint8_t buf[32] = {0};
        if (op.store) {
            if (op.vec) {
                memcpy(buf, vreg(cpu, op.rt), esize);
                if (pair) memcpy(buf + esize, vreg(cpu, op.rt2), esize);
            } else {
                uint64_t a = gpr(cpu, op.rt), b = pair ? gpr(cpu, op.rt2) : 0;
                memcpy(buf, &a, esize);
                if (pair) memcpy(buf + esize, &b, esize);
            }
            /* Every store of the kernel's early initialisation, in order: what it writes, where.
               A store that lands on the slot the halt comes from, or one that should have and did
               not, is the answer to why it later reads a non-pointer. */
            static uint32_t traced;
            if (cpu->m->log >= 2 && pc >= 0xfffffe0000000000ull && traced < 800) {
                uint64_t v0 = 0, v1 = 0;
                memcpy(&v0, buf, op.bytes >= 8 ? 8 : op.bytes);
                if (pair) memcpy(&v1, buf + esize, op.bytes > esize ? (op.bytes - esize) : 0);
                fprintf(stderr, "hengevm: store %llx <- %llx%s size %u pc %llx\n",
                        (unsigned long long) va, (unsigned long long) v0,
                        pair ? "" : "", op.bytes, (unsigned long long) pc);
                (void) v1;
                traced++;
            }
            rc = hv_vmem_xfer(cpu, va, buf, op.bytes, 1, el, pc);
            break;
        }
        rc = hv_vmem_xfer(cpu, va, buf, op.bytes, 0, el, pc);
        if (rc) break;
        if (op.vec) {
            memset(vreg(cpu, op.rt), 0, 16);
            memcpy(vreg(cpu, op.rt), buf, esize);
            if (pair) {
                memset(vreg(cpu, op.rt2), 0, 16);
                memcpy(vreg(cpu, op.rt2), buf + esize, esize);
            }
        } else {
            uint64_t a = extend(le(buf, esize > 8 ? 8 : esize), &op);
            uint64_t b = pair ? extend(le(buf + esize, esize), &op) : 0;
            /* Writeback first, so a loaded base register keeps the loaded value. */
            if (op.mode == HV_ADDR_PRE) set_base(cpu, op.rn, va);
            else if (op.mode == HV_ADDR_POST) set_base(cpu, op.rn, base + op.imm);
            set_gpr(cpu, op.rt, a);
            if (pair) set_gpr(cpu, op.rt2, b);
            return 0;
        }
        break;
    }
    }
    if (rc) return 1;
    if (op.mode == HV_ADDR_PRE) {
        set_base(cpu, op.rn, va);
    } else if (op.mode == HV_ADDR_POST) {
        int vec_reg = (op.kind == HV_MEM_VEC_MULTI || op.kind == HV_MEM_VEC_SINGLE) && op.rm != 31;
        set_base(cpu, op.rn, base + (vec_reg ? gpr(cpu, op.rm) : (uint64_t)op.imm));
    }
    return 0;
}
