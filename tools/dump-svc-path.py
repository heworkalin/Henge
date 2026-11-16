#!/usr/bin/env python3
# List the file paths the engine recorded for guest syscalls.
#
# hv_svc_path[] is filled per syscall with the path argument the engine could
# resolve. Pc/imm turned out to be garbage in this build, but the path strings
# are written independently, so the set of paths is still the record of what the
# guest opened -- which dylibs it loaded, which files it touched.
#
#   gdb -p <pid> -batch -x tools/dump-svc-path.py
import gdb

CAP = 4096
n = int(gdb.parse_and_eval("hv_svc_n"))
print("hv_svc_n = %d" % n)
seen = []
for i in range(min(n, CAP)):
    try:
        s = gdb.execute("x/s &hv_svc_path[%d]" % i, to_string=True)
    except gdb.error:
        continue
    if '"' not in s:
        continue
    t = s.split('"')[1] if s.count('"') >= 2 else ""
    if t:
        seen.append(t)
print("recorded paths: %d" % len(seen))
for t in seen:
    print("  " + t)
