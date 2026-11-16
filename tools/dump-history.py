#!/usr/bin/env python3
# Dump what the guest actually did, from the rings the engine already keeps:
# the last syscalls (hv_svc_*), the last userland blocks (hv_el0_trace_*) and
# the last EL0 state (hv_last_el0_*). Read-only, no engine change.
#
#   gdb -p $(pidof host-install) -batch -ex "set print thread-events off" -x tools/dump-history.py

import gdb

SVC_CAP, EL0_CAP = 4096, 16384
SYS = {1: "exit", 2: "fork", 3: "read", 4: "write", 5: "open", 6: "close",
       59: "execve", 73: "munmap", 92: "fcntl", 153: "unknown153",
       197: "mmap", 338: "unknown338"}


def u64(name):
    return int(gdb.parse_and_eval(name))


def main():
    el0n = u64("hv_el0_trace_n")
    svcn = u64("hv_svc_n")
    print("=== totals ===")
    print("el0 blocks ever run : %d" % el0n)
    print("syscalls ever made  : %d" % svcn)
    print("first el0 va        : 0x%x" % u64("hv_el0_trace_first_va"))
    print("last el0 va         : 0x%x" % u64("hv_last_el0_va"))
    print("last el0 page       : va 0x%x pa 0x%x" % (u64("hv_last_el0_page_va"),
                                                     u64("hv_last_el0_page_pa")))
    print("last el0 regs       : x0 0x%x x1 0x%x x2 0x%x x3 0x%x x8 0x%x x16 0x%x lr 0x%x sp 0x%x"
          % tuple(u64("hv_last_el0_" + r) for r in ("x0", "x1", "x2", "x3", "x8", "x16", "lr", "sp")))
    print("cpus el             : %d %d" % (u64("g_cpus[0].el"), u64("g_cpus[1].el")))
    print("cpus pc             : 0x%x 0x%x" % (u64("g_cpus[0].pc"), u64("g_cpus[1].pc")))

    print("\n=== last %d syscalls ===" % min(40, svcn))
    for i in range(max(0, svcn - 40), svcn):
        s = i & (SVC_CAP - 1)
        pc = u64("hv_svc_pc[%d]" % s)
        imm = int(gdb.parse_and_eval("hv_svc_imm[%d]" % s))
        x16 = u64("hv_svc_x16[%d]" % s)
        ret = u64("hv_svc_ret[%d]" % s)
        try:
            path = gdb.parse_and_eval("hv_svc_path[%d]" % s).string()
        except gdb.error:
            path = ""
        print("  #%d pc 0x%-12x svc %-4x x16 %-5d ret 0x%-8x %s"
              % (i, pc, imm, x16, ret, SYS.get(x16, "")))
        if path:
            print("        \"%s\"" % path)

    print("\n=== last %d el0 blocks ===" % min(40, el0n))
    for i in range(max(0, el0n - 40), el0n):
        s = i & (EL0_CAP - 1)
        print("  #%d va 0x%x x16 0x%x" % (i, u64("hv_el0_trace_va[%d]" % s),
                                          u64("hv_el0_trace_x16[%d]" % s)))


main()
