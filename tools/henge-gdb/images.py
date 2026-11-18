#!/usr/bin/env python3
# Name the userland image a PC belongs to, and resolve a GOT slot when given one.
#
#   HENGE_PC=0x100f9c5bc            address to identify (default: last EL0 pc)
#   HENGE_IMAGE_RANGE=0x100000000:0x103200000   where to look for Mach-O headers
#   HENGE_GOT_PC=0x100f9c5bc        adrp PC whose page holds the GOT target
#   HENGE_GOT_OFF=0x608             offset inside that page
#   gdb -p <pid> -batch -x tools/henge-gdb/images.py
#
# The stall kept landing next to an adrp/GOT load, so "which image, and is the
# slot filled?" is worth a one-shot answer (docs/userland-runs-but-stops-*.md).

exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

PC = arg("HENGE_PC", None)
if PC is None:
    PC = i64("hv_last_el0_va") or i64("hv_uexc_pc[0]") or 0
RNG = arg_range("HENGE_IMAGE_RANGE", 0x4000)
LO, HI, STEP = RNG if RNG else (0x100000000, 0x103200000, 0x4000)

MAGICS = (0xfeedfacf, 0xfeedface, 0xcafebabe)

TTBR = arg("HENGE_TTBR0", None)
if TTBR is None:
    TTBR = i64("hv_last_el0_ttbr0") or None
    if TTBR is None:
        n = i64("hv_el0_flt_n") or 0
        if n:
            TTBR = i64("hv_el0_flt_ttbr0[%d]" % ((n - 1) % 8)) or None
if not TTBR:
    print("no user ttbr0 known; pass HENGE_TTBR0=0x... (the CPU's is the kernel's)")
    raise SystemExit


def macho_text(pa):
    """(__TEXT vmaddr, vmsize) of a Mach-O header at PA, or None."""
    h = try_rd_pa(pa, 0x20)
    if not h or struct.unpack("<I", h[:4])[0] not in MAGICS:
        return None
    ncmds = struct.unpack("<I", h[16:20])[0]
    if ncmds == 0 or ncmds > 5000:
        return None
    buf = try_rd_pa(pa, 0x2000)
    if not buf:
        return None
    o = 32
    for _ in range(ncmds):
        if o + 8 > len(buf):
            return None
        c, sz = struct.unpack("<2I", buf[o:o + 8])
        if sz == 0:
            return None
        if c == 0x19 and o + 24 <= len(buf):
            name = buf[o + 8:o + 24].rstrip(b"\0")
            if name == b"__TEXT" and o + 56 <= len(buf):
                vmaddr, vmsize = struct.unpack("<2Q", buf[o + 24:o + 40])
                return vmaddr, vmsize
        o += sz
    return None


print("looking for the image that covers 0x%x in [0x%x,0x%x)  ttbr0=0x%x"
      % (PC, LO, HI, TTBR))
hits = []
mapped = 0
for va in range(LO, HI, STEP):
    pa = walk_ttbr(va, TTBR)
    if pa is None:
        continue
    mapped += 1
    t = macho_text(pa)
    if t and t[0] <= PC < t[0] + t[1]:
        hits.append((va, pa, t))
print("covering Mach-O images: %d  (%d of %d sampled pages resident)"
      % (len(hits), mapped, (HI - LO) // STEP))
if not hits and mapped == 0:
    print("  note: no page in range is mapped at all -- the image is registered but")
    print("        its PTEs are zero, so there is no header to name (see entries.py)")
for va, pa, (vm, sz) in hits:
    print("   image header VA=0x%x PA=0x%x  __TEXT vmaddr=0x%x size=0x%x  (pc offset +0x%x)"
          % (va, pa, vm, sz, PC - vm))

GOT_PC = arg("HENGE_GOT_PC", None)
if GOT_PC is not None:
    off = arg("HENGE_GOT_OFF", 0)
    page = GOT_PC & ~0xfff
    imm = arg("HENGE_GOT_IMM_PAGES", 0)
    target = page + imm * 0x1000
    slot = target + off
    print("\nadrp pc=0x%x -> page 0x%x (+%d pages) slot=0x%x"
          % (GOT_PC, target, imm, slot))
    pa = walk_ttbr(slot, TTBR)
    print("   slot PA = %s" % (("0x%x" % pa) if pa else "UNMAPPED"))
    if pa is not None:
        buf = try_rd_pa(pa, 64)
        if buf:
            for i, v in enumerate(struct.unpack("<8Q", buf)):
                print("   slot[0x%x] = 0x%016x%s"
                      % (off + i * 8, v, "  (kernel ptr)" if kaddr(v) else ""))
