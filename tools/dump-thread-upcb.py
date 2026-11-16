#!/usr/bin/env python3
# Find each blocked thread's saved userspace pc.
#
# A thread that blocked in the kernel has its userspace context parked in
# thread->machine.upcb (arm_kernel_context_t): kc_lr and kc_sp are kernel
# addresses, kc_pc is the userspace one. The earlier scan only listed kernel
# pointers (0xff..), which skips kc_pc entirely -- the one value that says which
# userspace instruction the thread stopped at.
#
#   gdb -p <pid> -batch -x tools/dump-thread-upcb.py
exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

TH = {0x01: "WAIT", 0x02: "SUSP", 0x04: "RUN", 0x08: "UNINT", 0x10: "TERM",
      0x20: "TERM2", 0x40: "WAITREP", 0x80: "IDLE", 0x100: "WAKING"}
USTART, UEND = 0x100000000, 0x10000000000      # arm64 userspace range


def bits(v):
    return "+".join(n for b, n in TH.items() if v & b) or "-"


def kaddr(v):
    return v >> 56 in (0xff, 0xfe)


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
    print("\nlaunchd%d 0x%x state=0x%x(%s)" % (i, t, st, bits(st)))
    for off in range(0, 0x2000, 8):
        v = p64(p + off)
        if USTART <= v < UEND:
            print("  +0x%04x = 0x%x  (userspace address)" % (off, v))
