#!/usr/bin/env python3
# Print the last syscalls the engine recorded before the stall.
#
# The engine keeps hv_svc_pc/imm/x0/path rings for every guest syscall. The tail
# is the guest's last activity, which is what the deadlock interrupted; the
# syscall number and path say which operation left the lock held.
#
#   gdb -p <pid> -batch -x tools/dump-last-svc.py
import gdb

n = int(gdb.parse_and_eval("hv_svc_n"))
print("hv_svc_n = %d" % n)
lo = max(0, n - 40)
try:
    for i in range(lo, n):
        pc = int(gdb.parse_and_eval("hv_svc_pc[%d]" % i))
        imm = int(gdb.parse_and_eval("hv_svc_imm[%d]" % i))
        x0 = int(gdb.parse_and_eval("hv_svc_x0[%d]" % i))
        ret = int(gdb.parse_and_eval("hv_svc_ret[%d]" % i))
        try:
            path = gdb.execute("x/s hv_svc_path[%d]" % i, to_string=True)
            path = path.split(":", 1)[1].strip() if ":" in path else path.strip()
        except Exception:
            path = "?"
        print("  [%5d] pc=0x%x nr=%d x0=0x%x ret=0x%x %s"
              % (i, pc, imm, x0, ret, path))
except Exception as e:
    print("error: %s" % e)
