#!/usr/bin/env python3
# Resolve every kernel pointer inside the turnstiles our waiters sit on.
#
# ts_thread was a guess from one earlier capture and does not survive: at
# +0x350 this capture reads 0. Instead of assuming a field offset, walk the
# whole turnstile and classify each word: known thread, known turnstile, or a
# kernel pointer whose target we then read a few words from. A lock's owner is
# in there somewhere, and the classification says where.
#
#   gdb -p <pid> -batch -x tools/dump-ts.py

exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

TH = {0x01: "WAIT", 0x02: "SUSP", 0x04: "RUN", 0x08: "UNINT", 0x10: "TERM",
      0x20: "TERM2", 0x40: "WAITREP", 0x80: "IDLE", 0x100: "WAKING"}


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
idle = [int(gdb.parse_and_eval("g_cpus[%d].tpidr_el1" % i)) for i in (0, 1)]
known = {t: "launchd%d" % i for i, t in enumerate(threads)}
for i, t in enumerate(idle):
    known[t] = "idle%d" % i
print("known threads: " + ", ".join("%s=0x%x" % (v, k2) for k2, v in known.items()))

# Every distinct turnstile the waiters point at.
tss = []
for t in threads + idle:
    p = walk(t)
    if p is None:
        continue
    st = p64(p + 0x1c8) & 0xffffffff
    ts = p64(p + 0x28)
    print("\n%s 0x%x state=0x%x(%s) turnstile=0x%x" % (known[t], t, st, bits(st), ts))
    if kaddr(ts) and ts not in tss:
        tss.append(ts)

for ts in tss:
    tsp = walk(ts)
    print("\n===== turnstile 0x%x (pa 0x%x) =====" % (ts, tsp))
    if tsp is None:
        print("  unmapped")
        continue
    for off in range(0, 0x400, 8):
        try:
            v = p64(tsp + off)
        except Exception:
            break
        if not kaddr(v):
            if v:
                print("  +0x%03x = 0x%x" % (off, v))
            continue
        tag = known.get(v)
        if tag is None:
            vp = walk(v)
            if vp is None:
                print("  +0x%03x = 0x%x  -> unmapped" % (off, v))
                continue
            w0, w1, w2 = p64(vp), p64(vp + 8), p64(vp + 16)
            tag = "kptr? w0=0x%x w1=0x%x w2=0x%x" % (w0, w1, w2)
        print("  +0x%03x = 0x%x  -> %s" % (off, v, tag))
