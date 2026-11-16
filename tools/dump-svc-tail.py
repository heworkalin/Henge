#!/usr/bin/env python3
# The last syscall paths, in order: where the guest was when it stopped.
#
#   gdb -p <pid> -batch -x tools/dump-svc-tail.py
import gdb

CAP = 4096
n = int(gdb.parse_and_eval("hv_svc_n"))
lo = max(0, min(n, CAP) - 70)
print("hv_svc_n = %d, showing %d..%d" % (n, lo, n - 1))
for i in range(lo, min(n, CAP)):
    try:
        s = gdb.execute("x/s &hv_svc_path[%d]" % i, to_string=True)
    except gdb.error:
        continue
    t = s.split('"')[1] if s.count('"') >= 2 else ""
    if t:
        print("  [%4d] %s" % (i, t))
