# Guest address translation for gdb attached to the engine.
#
# The engine's RAM slots are host mappings of guest physical memory, so a guest
# VA has to be walked through the guest's own tables before it can be read.
# Pasted into gdb scripts through exec(); the report below the TH table is not needed by them.
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


def cpu(n):
    if n not in _C:
        _C[n] = int(gdb.parse_and_eval("g_cpus[0].%s" % n))
    return _C[n]


def rd_pa(pa, n):
    for h, b, sz in S:
        if b <= pa and pa + n <= b + sz:
            return bytes(INF.read_memory(h + pa - b, n))
    raise ValueError("pa 0x%x not in any RAM slot" % pa)


def gb(tg, hi):
    return (14 if tg == 1 else 16 if tg == 3 else 12) if hi else \
           (14 if tg == 2 else 16 if tg == 1 else 12)


def walk(va):
    """Guest VA -> PA, or None when unmapped. Identity before the MMU is on."""
    if not (cpu("sctlr_el1") & 1):
        return va
    tcr = cpu("tcr_el1")
    hi = (va >> 55) & 1
    tsz = max(16, min(39, (tcr >> 16) & 63 if hi else tcr & 63))
    g = gb((tcr >> 30) & 3 if hi else (tcr >> 14) & 3, hi)
    inp = 64 - tsz
    st = g - 3
    lv = (inp - g + st - 1) // st
    first = 4 - lv
    level = first
    base = (cpu("ttbr1_el1") if hi else cpu("ttbr0_el1")) & OA & ~1
    while True:
        sh = g + st * (3 - level)
        ib = inp - sh if level == first else st
        idx = (va >> sh) & ((1 << ib) - 1)
        try:
            d = struct.unpack("<Q", rd_pa(base + idx * 8, 8))[0]
        except Exception:
            return None
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


def r64va(va):
    p = walk(va)
    return None if p is None else struct.unpack("<Q", rd_pa(p, 8))[0]


def p64(pa):
    return struct.unpack("<Q", rd_pa(pa, 8))[0]


def find_pcomm(name=b"launchd"):
    """Guest PA of p_comm for the process whose comm is `name`."""
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
                try:
                    if rd_pa(pa + 17, len(name)) == name:
                        return pa
                except ValueError:
                    pass
                i = buf.find(name, i + 1)
            off += n
    return None


def find_proc(pc):
    """(offset, proc PA, proc VA) from a p_comm PA, by requiring the list node
    at proc+offset to point back at it through its neighbours."""
    for k in range(pc % 8 or 8, 0x1000, 8):
        s = pc - k
        try:
            nxt = p64(s)
            prev = p64(s + 8)
        except ValueError:
            break
        if not (nxt >> 56 in (0xff, 0xfe) and prev >> 56 in (0xff, 0xfe)):
            continue
        pp = walk(prev)
        if pp is None:
            continue
        sv = p64(pp)
        if sv >> 56 in (0xff, 0xfe) and walk(sv) == s:
            return k, s, sv
    return None


TH = {0x01: "WAIT", 0x02: "SUSP", 0x04: "RUN", 0x08: "UNINT", 0x10: "TERM",
      0x20: "TERM2", 0x40: "WAITREP", 0x80: "IDLE", 0x100: "WAKING"}
