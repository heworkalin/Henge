#!/usr/bin/env python3
# Find each waiter's kernel stack and print the code addresses on it.
#
# The turnstile tells us they are all parked on one lock, not where. The kernel
# stack does: a blocked thread still has its call chain on it, so the return
# addresses name the functions it went through. arm64 kernel stacks are 16 KiB
# and 16 KiB aligned (KERNEL_STACK_SIZE), so a per-thread scan for aligned
# kernel pointers that contain code addresses finds them without knowing the
# offset of kernel_stack inside struct thread, which differs between releases.
#
#   gdb -p <pid> -batch -x tools/dump-stacks.py

exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

TH = {0x01: "WAIT", 0x02: "SUSP", 0x04: "RUN", 0x08: "UNINT", 0x10: "TERM",
      0x20: "TERM2", 0x40: "WAITREP", 0x80: "IDLE", 0x100: "WAKING"}
KSSIZE = 0x4000
TEXT_LO, TEXT_HI = 0xfffffe0010000000, 0xfffffe0040000000   # guest kernel text


def bits(v):
    return "+".join(n for b, n in TH.items() if v & b) or "-"


def kaddr(v):
    return v >> 56 in (0xff, 0xfe)


def is_text(v):
    return TEXT_LO <= v < TEXT_HI


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
    best = None
    for off in range(0, 0x1800, 8):
        v = p64(tp + off)
        if not (kaddr(v) and v % 0x1000 == 0):
            continue
        # kernel_stack points at the top of the stack; read the block below it
        for start in (v - KSSIZE, v):
            vp = walk(start)
            if vp is None:
                continue
            try:
                buf = bytes(INF.read_memory(vp, KSSIZE))
            except Exception:
                continue
            hits = []
            for j in range(0, KSSIZE, 8):
                w = int.from_bytes(buf[j:j + 8], "little")
                if is_text(w):
                    hits.append((j, w))
            if hits and (best is None or len(hits) > len(best[2])):
                best = (off, start, hits)
            break
    print("\n===== launchd%d 0x%x state=0x%x(%s)" % (i, t, st, bits(st)))
    if best is None:
        print("  no kernel stack found")
        continue
    off, va, hits = best
    print("  stack: thread+0x%x -> 0x%x, %d code addresses (top 24)"
          % (off, va, len(hits)))
    for j, w in hits[:24]:
        print("    sp+0x%04x  0x%x" % (j, w))
