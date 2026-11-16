#!/usr/bin/env python3
# gdb (embedded Python) probe: find launchd's struct proc / task / threads in
# a Henge guest, from guest physical memory only, and without trusting a single
# hand-computed struct offset.
#
# Why it is written this way. struct proc has ~200 fields behind #if's, so any
# offset "read off the header" is a guess until it is checked. Two structural
# facts are not guesses, though:
#
#   * `struct proc` starts with `union { LIST_ENTRY(proc) p_list; ... }`
#     (bsd/sys/proc_internal.h), so a proc's address *is* its p_list;
#   * `p_comm` is a command_t (char[17]) immediately followed by `p_name`,
#     a proc_name_t (char[33]) (osfmk/mach/resource_monitors.h), and both hold
#     "launchd".
#
# So: find the one "launchd\0 <17 bytes> launchd\0" run, then walk back to the
# byte offset where a LIST_ENTRY makes sense -- and *prove* it, with the LIST's
# own invariant. For a node S with p_list.le_prev = V, V points at the le_next
# field that holds S's address, so:
#
#     walk(read64(walk(V))) == S
#
# That chain is checkable without knowing S's virtual address in advance, and
# it is what makes the offset below evidence rather than derivation. The same
# trick locates the task (task->bsd_info points back at the proc) and the
# threads (thread->task points back at the task).
#
# Run under the bundle's gdb (guest paused while it runs):
#   gdb -p $(pidof host-install) -batch -ex "set print thread-events off" \
#       -x /data/local/tmp/henge/probe_xnu.py

import struct

import gdb

INF = gdb.selected_inferior()
OA_MASK = 0x0000ffffffffffff
CMD = 17   # command_t
NAME = 33  # proc_name_t


# ---------------------------------------------------------------- plumbing

class Unmapped(Exception):
    pass


def ram_segments():
    """(host, pa, size) for every segment g_machine knows."""
    segs = []
    for i in range(int(gdb.parse_and_eval("g_machine.nram"))):
        segs.append((
            int(gdb.parse_and_eval("g_machine.ram[%d].host" % i)),
            int(gdb.parse_and_eval("g_machine.ram[%d].pa" % i)),
            int(gdb.parse_and_eval("g_machine.ram[%d].size" % i)),
        ))
    return segs


SEGS = ram_segments()


def rd_pa(pa, n):
    """Bytes at a guest physical address, through whichever segment holds it."""
    for host, base, size in SEGS:
        if base <= pa and pa + n <= base + size:
            try:
                return bytes(INF.read_memory(host + (pa - base), n))
            except gdb.MemoryError:
                raise Unmapped("pa 0x%x" % pa)
    raise Unmapped("pa 0x%x" % pa)


def rd64(pa):
    return struct.unpack("<Q", rd_pa(pa, 8))[0]


def rd32(pa):
    return struct.unpack("<I", rd_pa(pa, 4))[0]


def rd_sz(pa, n):
    return rd_pa(pa, n).split(b"\0")[0].decode("latin-1")


_CPU = {}


def cpu(name):
    """A CPU register, read once: the walk below asks for these thousands of times."""
    if name not in _CPU:
        _CPU[name] = int(gdb.parse_and_eval("g_cpus[0].%s" % name))
    return _CPU[name]


def granule_bits(tg, ttbr1):
    if ttbr1:
        return 14 if tg == 1 else 16 if tg == 3 else 12
    return 14 if tg == 2 else 16 if tg == 1 else 12


def walk(va):
    """A copy of hw_mmu_walk (core/mmu.c) for translation only: pa or None.

    Reimplemented rather than called because gdb's `call` runs inside the
    stopped inferior: any lock the engine or libc holds would deadlock it, and
    a 48-byte out-parameter would need an allocation to point at.
    """
    if not (cpu("sctlr_el1") & 1):
        return va
    tcr = cpu("tcr_el1")
    hi = (va >> 55) & 1
    tsz = (tcr >> 16) & 63 if hi else tcr & 63
    epd = (tcr >> 23) & 1 if hi else (tcr >> 7) & 1
    g = granule_bits((tcr >> 30) & 3 if hi else (tcr >> 14) & 3, hi)
    if tsz < 16:
        tsz = 16
    if tsz > 39:
        tsz = 39
    if epd:
        return None
    inputsize = 64 - tsz
    stride = g - 3
    levels = (inputsize - g + stride - 1) // stride
    first = 4 - levels
    level = first
    base = (cpu("ttbr1_el1") if hi else cpu("ttbr0_el1")) & OA_MASK & ~1
    while True:
        shift = g + stride * (3 - level)
        idx_bits = inputsize - shift if level == first else stride
        if idx_bits <= 0:
            return None
        idx = (va >> shift) & ((1 << idx_bits) - 1)
        try:
            desc = rd64(base + idx * 8)
        except Unmapped:
            return None
        if not (desc & 1):
            return None
        if level < 3 and (desc & 2):
            base = desc & OA_MASK & ~((1 << g) - 1)
            level += 1
            continue
        if level == 3 and not (desc & 2):
            return None
        if level < 3 and (level == 0 or (g != 12 and level == 1)):
            return None
        size = 1 << shift
        return (desc & OA_MASK & ~(size - 1)) | (va & (size - 1))


def rd_va(va, n):
    """Bytes at a guest virtual address, or None."""
    pa = walk(va)
    if pa is None:
        return None
    try:
        return rd_pa(pa, n)
    except Unmapped:
        return None


def kw(va):
    """The 8-byte word at va, or None."""
    b = rd_va(va, 8)
    return None if b is None else struct.unpack("<Q", b)[0]


def is_kaddr(v):
    return v is not None and (v >> 56) in (0xff, 0xfe)


def dump(pa, n, mark_pa=None):
    data = rd_pa(pa, n)
    for row in range(0, len(data) - 15, 16):
        a = pa + row
        cells = " ".join("%02x" % c for c in data[row:row + 16])
        txt = "".join(chr(c) if 32 <= c < 127 else "." for c in data[row:row + 16])
        star = " <--" if mark_pa is not None and a <= mark_pa < a + 16 else ""
        print("    %011x  %-47s |%s|%s" % (a, cells, txt, star))


# ---------------------------------------------------------------- locating

def find_name_blocks(pat=b"launchd"):
    """Physical addresses where `p_comm` and `p_name` both read `pat`."""
    hits = []
    for host, base, size in SEGS:
        if size < CMD + NAME:
            continue
        off = 0
        chunk = 4 << 20
        while off < size:
            n = min(chunk, size - off)
            try:
                buf = bytes(INF.read_memory(host + off, n))
            except gdb.MemoryError:
                off += n
                continue
            i = buf.find(pat)
            while i >= 0:
                pa = base + off + i
                # h is the p_comm start; p_name is CMD bytes later.
                try:
                    if rd_pa(pa + CMD, len(pat)) == pat:
                        hits.append(pa)
                except Unmapped:
                    pass
                i = buf.find(pat, i + 1)
            off += n
    return hits


def proc_start_of(pcomm_pa):
    """The byte offset K where p_comm sits in a proc that starts at pcomm_pa-K.

    Proven by the LIST invariant: for p_list.le_prev = V,
    walk(read64(walk(V))) must equal the proc's own address.
    """
    out = []
    # A zone hands out 8-byte-aligned objects, so the proc's own start is
    # aligned: the distance back to p_comm must share p_comm's residue mod 8.
    # Reading at any other offset lands mid-field and produces the plausible
    # nonsense this probe printed before (a qword built from two half fields).
    first = pcomm_pa % 8 or 8
    for k in range(first, 0x1000, 8):
        s_pa = pcomm_pa - k
        try:
            nxt, prev = rd64(s_pa), rd64(s_pa + 8)
        except Unmapped:
            break
        if not is_kaddr(prev):
            continue
        prev_pa = walk(prev)
        if prev_pa is None:
            continue
        try:
            s_va_guess = rd64(prev_pa)
        except Unmapped:
            continue
        if not is_kaddr(s_va_guess):
            continue
        if walk(s_va_guess) != s_pa:
            continue
        out.append((k, s_pa, s_va_guess, nxt, prev))
    return out


def main():
    print("ram: " + "  ".join("pa 0x%x size 0x%x host 0x%x" % (pa, size, host)
                              for host, pa, size in SEGS))
    print("cpu0: sctlr 0x%x tcr 0x%x ttbr1 0x%x"
          % (cpu("sctlr_el1"), cpu("tcr_el1"), cpu("ttbr1_el1")))

    # The walk itself, against the two image addresses the project documents
    # (docs/kernel-boot-exclusions.md): if these translate, the walk is right.
    for va, want in ((0xfffffe001bb18000, 0x73b18000),
                     (0xfffffe001c890000, 0x74090000)):
        got = walk(va)
        print("  walk 0x%x -> %s (expect 0x%x) %s"
              % (va, "0x%x" % got if got is not None else "unmapped", want,
                 "OK" if got == want else "MISMATCH (slide differs?)"))

    hits = find_name_blocks()
    print("\np_comm+p_name blocks: %d" % len(hits))

    for h in hits:
        cands = proc_start_of(h)
        if not cands:
            continue
        print("\nlaunchd p_comm at pa 0x%x, %d LIST-consistent proc starts" % (h, len(cands)))
        for k, s_pa, s_va, nxt, prev in cands:
            print("  p_comm offset 0x%x -> proc pa 0x%x va 0x%x"
                  "  p_list.next 0x%x prev 0x%x" % (k, s_pa, s_va, nxt, prev))
            # p_pid is the int32 == 1 nearest before the name block, and
            # p_ppid (0 for launchd) should be a few bytes before it.
            back = 0x100
            region = rd_pa(s_pa, back + (h - s_pa))
            pids = [i for i in range(0, len(region) - 3, 4)
                    if struct.unpack_from("<i", region, i)[0] == 1]
            print("    int32==1 at proc%+d: %s"
                  % (0, ["+0x%x" % (i - 0) if i < h - s_pa else "?" for i in pids][:8]))
        dump(h - 0x60, 0x100, h)
        break   # one proc is enough; a second run would be the same object


main()
