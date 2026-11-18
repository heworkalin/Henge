# Guest address translation and structure lookup for gdb attached to the engine.
#
# The engine's RAM slots are host mappings of guest physical memory, so a guest
# VA has to be walked through the guest's own tables before it can be read.
# The granule comes from TCR (VMAPPLE guests use 16 KiB, TG0=2) -- never assume
# 4 KiB. Structure offsets are XNU's for xnu-11417.140.69; see
# docs/vm-map-structure-2026-11-17.md.
#
# Nothing here is tied to one run: addresses come from the environment.
#
#   HENGE_VA=0x100fc0000            one VA       (also: comma separated list)
#   HENGE_RANGE=lo:hi[:step]        a VA sweep   (default step = one page)
#   HENGE_MAP_PA=0x83fb8220         skip discovery, use this vm_map
#   HENGE_PMAP=0xfffffe1e55f54170   skip discovery, use this pmap
#   HENGE_COMM=launchd              process name to locate (default launchd)
#   HENGE_FAULTS=8                  fault-ring slots to print
#
# Pasted into other scripts by taking everything above the TH status table, so
# everything up here must stay import-side-effect free.

import os
import struct

import gdb

INF = gdb.selected_inferior()
OA = 0x0000ffffffffffff
S = []
for _i in range(int(gdb.parse_and_eval("g_machine.nram"))):
    S.append((int(gdb.parse_and_eval("g_machine.ram[%d].host" % _i)),
              int(gdb.parse_and_eval("g_machine.ram[%d].pa" % _i)),
              int(gdb.parse_and_eval("g_machine.ram[%d].size" % _i))))

_C = {}


def arg(name, default=None, cast=int):
    """Environment parameter: decimal, 0x-hex, or "" (meaning unset)."""
    v = os.environ.get(name, "")
    if v == "":
        return default
    if cast is int:
        return int(v, 0)
    return cast(v)


def arg_list(name):
    """Comma/space separated list of integers (hex allowed)."""
    v = os.environ.get(name, "")
    return [int(x, 0) for x in v.replace(",", " ").split() if x]


def arg_range(name, page=0x4000):
    """lo:hi[:step] -> (lo, hi, step); step defaults to one page."""
    v = os.environ.get(name, "")
    if not v:
        return None
    p = [int(x, 0) for x in v.split(":")]
    if len(p) < 2:
        return None
    lo, hi = p[0], p[1]
    step = p[2] if len(p) > 2 else page
    return lo, hi, step or page


def cpu(n):
    if n not in _C:
        _C[n] = int(gdb.parse_and_eval("g_cpus[0].%s" % n))
    return _C[n]


def i64(n):
    try:
        return int(gdb.parse_and_eval(n))
    except Exception:
        return None


def rd_pa(pa, n):
    for h, b, sz in S:
        if b <= pa and pa + n <= b + sz:
            return bytes(INF.read_memory(h + pa - b, n))
    raise ValueError("pa 0x%x not in any RAM slot" % pa)


def try_rd_pa(pa, n):
    try:
        return rd_pa(pa, n)
    except Exception:
        return None


def gb(tg, hi):
    return (14 if tg == 1 else 16 if tg == 3 else 12) if hi else \
           (14 if tg == 2 else 16 if tg == 1 else 12)


def walk_ttbr(va, ttbr0, tcr=None, sctlr=None, trail=None):
    """Guest VA -> PA, or None when unmapped. Optionally fills `trail`
    with (level, table_pa, index, descriptor) for every table read."""
    if sctlr is None:
        sctlr = cpu("sctlr_el1")
    if not (sctlr & 1):
        return va
    if tcr is None:
        tcr = cpu("tcr_el1")
    hi = (va >> 55) & 1
    tsz = max(16, min(39, (tcr >> 16) & 63 if hi else tcr & 63))
    g = gb((tcr >> 30) & 3 if hi else (tcr >> 14) & 3, hi)
    inp = 64 - tsz
    st = g - 3
    lv = (inp - g + st - 1) // st
    first = 4 - lv
    level = first
    base = (cpu("ttbr1_el1") if hi else ttbr0) & OA & ~1
    while True:
        sh = g + st * (3 - level)
        ib = inp - sh if level == first else st
        idx = (va >> sh) & ((1 << ib) - 1)
        try:
            d = struct.unpack("<Q", rd_pa(base + idx * 8, 8))[0]
        except Exception:
            return None
        if trail is not None:
            trail.append((level, base, idx, d))
        if not (d & 1):
            return None
        if level < 3 and (d & 2):
            base = d & OA & ~((1 << g) - 1)
            level += 1
            continue
        if level == 3 and not (d & 2):
            return None
        sz = 1 << sh
        return (d & OA & ~(sz - 1)) | (va & (sz - 1))


def walk(va):
    """VA -> PA using the engine's current view of this CPU."""
    return walk_ttbr(va, cpu("ttbr0_el1"))


def r64va(va):
    p = walk(va)
    return None if p is None else struct.unpack("<Q", rd_pa(p, 8))[0]


def p64(pa):
    return struct.unpack("<Q", rd_pa(pa, 8))[0]


def u32(pa):
    return struct.unpack("<I", rd_pa(pa, 4))[0]


def u16(pa):
    return struct.unpack("<H", rd_pa(pa, 2))[0]


def kaddr(v):
    return (v >> 56) in (0xff, 0xfe)


def hostaddr(va):
    """Host address gdb can read directly, for a guest VA (None if unmapped)."""
    pa = walk(va)
    if pa is None:
        return None
    for h, b, sz in S:
        if b <= pa < b + sz:
            return h + pa - b
    return None


def find_pcomm(name=None):
    """Guest PA of p_comm for the process whose comm is `name`."""
    if name is None:
        name = arg("HENGE_COMM", "launchd", str).encode()
    if isinstance(name, str):
        name = name.encode()
    for h, b, sz in S:
        off = 0
        while off < sz:
            n = min(4 << 20, sz - off)
            try:
                buf = bytes(INF.read_memory(h + off, n))
            except gdb.MemoryError:
                off += n
                continue
            i = buf.find(name)
            while i >= 0:
                pa = b + off + i
                if try_rd_pa(pa + 17, len(name)) == name:
                    return pa
                i = buf.find(name, i + 1)
            off += n
    return None


def find_proc(pc):
    """(list-node offset, proc PA, proc VA) from a p_comm PA, by requiring the
    list node at proc+offset to point back at it through its neighbours."""
    for k in range(pc % 8 or 8, 0x1000, 8):
        s = pc - k
        try:
            nxt = p64(s)
            prev = p64(s + 8)
        except ValueError:
            break
        if not (kaddr(nxt) and kaddr(prev)):
            continue
        pp = walk(prev)
        if pp is None:
            continue
        sv = p64(pp)
        if kaddr(sv) and walk(sv) == s:
            return k, s, sv
    return None


def task_threads(task_va, limit=64):
    """(task, thread_count, [thread VA]) for a task, via task->threads (+0x50)."""
    tpa = walk(task_va)
    if tpa is None:
        return None, 0, []
    head = p64(tpa + 0x50)
    ths = []
    cur = head
    for _ in range(limit):
        if cur is None or not kaddr(cur) or cur in ths:
            break
        ths.append(cur)
        p = walk(cur)
        if p is None:
            break
        cur = p64(p)
        if cur == head:
            break
    return task_va, len(ths), ths


def proc_task(comm=None):
    """(proc_va, task_va) for a process name, or (None, None)."""
    pc = find_pcomm(comm)
    if pc is None:
        return None, None
    r = find_proc(pc)
    if r is None:
        return None, None
    _k, ppa, pva = r
    ro_pa = walk(p64(ppa + 0x18))
    if ro_pa is None:
        return pva, None
    return pva, p64(ro_pa + 8)


def maps(min_entries=1, limit=None):
    """Discover vm_map structures by signature, independent of any task.

    Signature (VM_MAP_STORE_USE_RB builds, 16 KiB pages):
      +0x03 lck_rw_type == 0x33 (LCK_TYPE_RW), +0x04 padding == 0
      +0x20 min_offset < +0x28 max_offset, spanning the user range
      +0x30 nentries, +0x34 page_shift, +0x40 pmap (kernel heap pointer)
    Returns dict(pa, min, max, nentries, page_shift, pmap, pmap_off)."""
    PAT = b"\x33\x00\x00\x00\x00"
    out = []
    for h, pa0, sz in S:
        buf = INF.read_memory(h, sz).tobytes()
        off = 0
        while True:
            j = buf.find(PAT, off)
            if j < 0:
                break
            off = j + 1
            if j < 3 or j + 0x48 > len(buf):
                continue
            start = struct.unpack("<Q", buf[j + 0x1d:j + 0x25])[0]
            end = struct.unpack("<Q", buf[j + 0x25:j + 0x2d])[0]
            if not (start < end) or start > 0x1000000000 or end < 0x1000000000:
                continue
            nent = struct.unpack("<I", buf[j + 0x2d:j + 0x31])[0]
            if nent > 0x100000 or nent < min_entries:
                continue
            pmap = struct.unpack("<Q", buf[j + 0x3d:j + 0x45])[0]
            if not kaddr(pmap):
                continue
            out.append(dict(pa=pa0 + j - 3, min=start, max=end, nentries=nent,
                            page_shift=struct.unpack("<H", buf[j + 0x31:j + 0x33])[0],
                            pmap=pmap, pmap_off=0x40))
            if limit and len(out) >= limit:
                return out
    return out


def find_map_for(va, cands=None):
    """The vm_map whose [min,max) covers `va`."""
    for m in (cands if cands is not None else maps()):
        if m["min"] <= va < m["max"]:
            return m
    return None


def pmap_ttb0(pmap_va):
    """ttb0 of a pmap (pmap+0x08 in this XNU build)."""
    p = walk(pmap_va)
    return None if p is None else p64(p + 0x08)


def map_entries(map_pa, limit=4000):
    """Entries of a vm_map as [(entry_va, start, end)].

    Both store layouts are tried: RB root first (map+0x18), then the plain
    doubly linked header (map+0x08); entry links are {prev, next, start, end}."""
    for head_off in (0x18, 0x08):
        head = p64(map_pa + head_off)
        if not kaddr(head):
            continue
        ents = []
        cur = head
        seen = set()
        ok = True
        while len(ents) < limit:
            if cur in seen:
                break
            seen.add(cur)
            p = walk(cur)
            if p is None:
                ok = False
                break
            s, e = p64(p + 0x10), p64(p + 0x18)
            if not (s <= e):
                ok = False
                break
            ents.append((cur, s, e))
            nxt = p64(p + 0x08)
            if not kaddr(nxt) or nxt == head:
                break
            cur = nxt
        if ok and ents:
            return ents
    return []


def entry_for(va, map_pa, entries=None):
    for ent, s, e in (entries if entries is not None else map_entries(map_pa)):
        if s <= va < e:
            return ent, s, e
    return None


TH = dict()
TH.update({0x01: "WAIT", 0x02: "SUSP", 0x04: "RUN", 0x08: "UNINT", 0x10: "TERM",
           0x20: "TERM2", 0x40: "WAITREP", 0x80: "IDLE", 0x100: "WAKING"})


def th_bits(v):
    return "+".join(n for b, n in TH.items() if v & b) or "-"
