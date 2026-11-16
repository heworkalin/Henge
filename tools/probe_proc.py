#!/usr/bin/env python3
# gdb (embedded Python) probe: locate launchd's struct proc in guest RAM.
#
# XNU's struct proc holds `command_t p_comm` (char[17]) immediately followed by
# `proc_name_t p_name` (char[33]) [osfmk/mach/resource_monitors.h:75-76], and for
# launchd both hold "launchd". So a hit at h with "launchd" at h+17 is not a
# string constant: it is a proc's name block. Nothing in a ramdisk or a binary
# carries two adjacent, identically named 17-byte command buffers.
#
# Run under the bundle's gdb:
#   gdb -p $(pidof host-install) -batch -x probe_proc.py
#
# It reads guest RAM through /proc/<pid>/mem (gdb's inferior memory), so the
# guest is paused while it runs: a couple of seconds for 2 GB.

import struct

import gdb

CMD = 17          # sizeof(command_t)  = MAXCOMLEN + 1
NAME = 33         # sizeof(proc_name_t) = 2*MAXCOMLEN + 1
PAT = b"launchd"


def mem(addr, n):
    return bytes(gdb.selected_inferior().read_memory(addr, n))


def ram_segments():
    n = int(gdb.parse_and_eval("g_machine.nram"))
    out = []
    for i in range(n):
        host = int(gdb.parse_and_eval("g_machine.ram[%d].host" % i))
        size = int(gdb.parse_and_eval("g_machine.ram[%d].size" % i))
        pa = int(gdb.parse_and_eval("g_machine.ram[%d].pa" % i))
        out.append((host, pa, size))
    return out


def all_hits(host, size, pat, chunk=4 << 20):
    """Every offset of `pat` in the segment, in chunks the ptrace path can take."""
    hits = []
    off = 0
    while off < size:
        n = min(chunk, size - off)
        try:
            buf = mem(host + off, n)
        except gdb.MemoryError:
            off += n
            continue
        i = buf.find(pat)
        while i >= 0:
            hits.append(off + i)
            i = buf.find(pat, i + 1)
        off += n
    return hits


def ascii_of(b):
    return "".join(chr(c) if 32 <= c < 127 else "." for c in b)


def dump(base, data, mark):
    for row in range(0, len(data) - 15, 16):
        addr = base + row
        cells = " ".join("%02x" % c for c in data[row:row + 16])
        star = " <--" if mark is not None and addr <= mark < addr + 16 else ""
        print("    %012x  %-47s  |%s|%s"
              % (addr, cells, ascii_of(data[row:row + 16]), star))


def main():
    segs = ram_segments()
    print("ram: " + ", ".join("pa 0x%x size 0x%x host 0x%x" % s for s in segs))

    for host, pa, size in segs:
        if size < len(PAT):
            continue
        hits = all_hits(host, size, PAT)
        if not hits:
            continue

        # Structural filter: p_comm then p_name, both "launchd".
        cands = []
        for h in hits:
            try:
                # h is the p_comm start; p_name must hold the same string.
                if mem(host + h + CMD, len(PAT)) == PAT:
                    cands.append(h)
            except gdb.MemoryError:
                pass

        print("segment pa 0x%x: %d hits, %d with p_comm+p_name"
              % (pa, len(hits), len(cands)))

        for h in cands:
            print("\n  candidate p_comm at guest pa 0x%x" % (pa + h))
            lo = max(0, h - 0x40)
            hi = min(size, h + CMD + NAME + 0x40)
            dump(pa + lo, mem(host + lo, hi - lo), pa + h)

            # p_pid is a pid_t (int32) somewhere before p_comm, and launchd's is
            # 1. Every such word in the 512 bytes before the name block is a
            # candidate to be checked against the offsets XNU declares.
            back = min(0x200, h)
            region = mem(host + h - back, back)
            pids = []
            for i in range(0, back - 3, 4):
                if struct.unpack_from("<i", region, i)[0] == 1:
                    pids.append(i - back)   # offset relative to p_comm
            print("    int32 == 1 at p_comm%+d: %s" % (0, pids))


main()
