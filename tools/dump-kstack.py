#!/usr/bin/env python3
# Locate each blocked thread's kernel stack and dump it.
#
# struct machine_thread keeps kstackptr right after the saved context, and the
# stack is KERNEL_STACK_SIZE-aligned (16 KiB). Rather than trust an offset we do
# not have a symbol for, score every aligned kernel pointer inside the thread by
# how many kernel text addresses the 16 KiB at and below it contains: a stack
# holds return addresses, a heap object does not.
#
#   gdb -p <pid> -batch -x tools/dump-kstack.py
exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

TH = {0x01: "WAIT", 0x02: "SUSP", 0x04: "RUN", 0x08: "UNINT", 0x10: "TERM",
      0x20: "TERM2", 0x40: "WAITREP", 0x80: "IDLE", 0x100: "WAKING"}
KS = 0x4000
TEXT_LO, TEXT_HI = 0xfffffe0010000000, 0xfffffe0040000000


def bits(v):
    return "+".join(n for b, n in TH.items() if v & b) or "-"


def kaddr(v):
    return v >> 56 in (0xff, 0xfe)


def score(start_va):
    n = 0
    ok = False
    for base in range(start_va, start_va + KS, 0x1000):
        pa = walk(base)
        if pa is None:
            continue
        try:
            d = rd_pa(pa, 0x1000)
        except Exception:
            continue
        ok = True
        for i in range(0, 0x1000, 8):
            v = int.from_bytes(d[i:i + 8], "little")
            if TEXT_LO <= v < TEXT_HI:
                n += 1
    return n if ok else -1


pcomm = find_pcomm()
k, ppa, pva = find_proc(pcomm)
ro_pa = walk(p64(ppa + 0x18))
task = p64(ro_pa + 8)
tpa = walk(task)
head = p64(tpa + 0x50)
ths = []
cur = head
for _ in range(64):
    if cur is None or not kaddr(cur) or cur in ths:
        break
    ths.append(cur)
    p = walk(cur)
    if p is None:
        break
    cur = p64(p)
    if cur == head:
        break

for i, t in enumerate(ths):
    p = walk(t)
    st = p64(p + 0x1c8) & 0xffffffff
    best = None
    for off in range(0, 0x2000, 8):
        v = p64(p + off)
        if not (kaddr(v) and v % KS == 0):
            continue
        for start in (v - KS, v):
            sc = score(start)
            if sc > 0 and (best is None or sc > best[2]):
                best = (off, start, sc)
    print("\nlaunchd%d 0x%x state=0x%x(%s)" % (i, t, st, bits(st)))
    if best is None:
        print("  no stack-like pointer found")
        continue
    off, va, sc = best
    print("  thread+0x%x -> stack 0x%x (%d text addrs)" % (off, va, sc))
    addrs = []
    for base in range(va, va + KS, 0x1000):
        pa = walk(base)
        if pa is None:
            continue
        try:
            d = rd_pa(pa, 0x1000)
        except Exception:
            continue
        for j in range(0, 0x1000, 8):
            v = int.from_bytes(d[j:j + 8], "little")
            if TEXT_LO <= v < TEXT_HI:
                addrs.append((base + j, v))
    for a, v in addrs[:30]:
        print("    sp+0x%04x  0x%x" % (a - va, v))
