#!/usr/bin/env python3
# List every kernel pointer inside a blocked thread and score each target.
#
# dump-stacks.py found no page-aligned stack pointer, so the assumption about
# where struct thread keeps kernel_stack is wrong for this release. Report the
# facts instead: each kernel pointer in the thread, the first words of its
# target, and how many guest text addresses live in the 16 KiB at and below it.
# The stack is the entry with many code addresses; unscored entries tell us what
# the other fields are.
#
#   gdb -p <pid> -batch -x tools/dump-thread-kptrs.py

exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

TH = {0x01: "WAIT", 0x02: "SUSP", 0x04: "RUN", 0x08: "UNINT", 0x10: "TERM",
      0x20: "TERM2", 0x40: "WAITREP", 0x80: "IDLE", 0x100: "WAKING"}
KSSIZE = 0x4000
TEXT_LO, TEXT_HI = 0xfffffe0010000000, 0xfffffe0040000000


def bits(v):
    return "+".join(n for b, n in TH.items() if v & b) or "-"


def kaddr(v):
    return v >> 56 in (0xff, 0xfe)


def textcount(start_va):
    # Walk page by page: a stack block can straddle mappings, and one failing
    # bulk read would hide the whole region.
    n = 0
    ok = False
    for base in range(start_va, start_va + KSSIZE, 0x1000):
        vp = walk(base)
        if vp is None:
            continue
        try:
            buf = bytes(rd_pa(vp, 0x1000))   # walk() returns a PA
        except Exception:
            continue
        ok = True
        for j in range(0, 0x1000, 8):
            w = int.from_bytes(buf[j:j + 8], "little")
            if TEXT_LO <= w < TEXT_HI:
                n += 1
    return n if ok else -1


pcomm = find_pcomm()
k, ppa, pva = find_proc(pcomm)
ro_pa = walk(p64(ppa + 0x18))
task = p64(ro_pa + 8)
tpa = walk(task)
head = p64(tpa + 0x50)
threads = []
cur = head
for _ in range(32):
    if cur is None or not kaddr(cur) or cur in threads:
        break
    threads.append(cur)
    p = walk(cur)
    if p is None:
        break
    cur = p64(p)
    if cur == head:
        break

for i, t in enumerate(threads):
    tp = walk(t)
    if tp is None:
        continue
    st = p64(tp + 0x1c8) & 0xffffffff
    print("\n===== launchd%d 0x%x state=0x%x(%s) pa=0x%x" % (i, t, st, bits(st), tp))
    for off in range(0, 0x2000, 8):
        v = p64(tp + off)
        if not kaddr(v) or v in threads:
            continue
        vp = walk(v)
        if vp is None:
            continue
        try:
            w0, w1, w2, w3 = p64(vp), p64(vp + 8), p64(vp + 16), p64(vp + 24)
        except Exception:
            continue
        n = max(textcount(v), textcount(v - KSSIZE))
        print("  +0x%04x = 0x%x  w=0x%x 0x%x 0x%x 0x%x  text=%d"
              % (off, v, w0, w1, w2, w3, n))
