#!/usr/bin/env python3
"""Offline analysis of an HV_TRACE_PC file (one u64 run VA per translated block).

    HV_TRACE_PC=<file> ./host-install ...            # record
    tools/trace-stats.py <file> --range 0x1cfff000:0x1d000000

Why this exists: HV_PCHK counts are not call counts (chaining/superblocks skip
the checkpoint), but the trace file records every block that actually ran, in
order. So "did the guest ever enter F" and "how many times, and when relative
to the page faults" are both answerable here and only here.

The file holds run VAs (link VA + slide), so pass --slide when a range is given
in link addresses straight from tools/lcfs.py.
"""
import argparse
import collections
import mmap
import os
import struct
import sys


def parse_range(s):
    lo, _, hi = s.partition(":")
    return int(lo, 0), int(hi, 0)


def fit_kernel(lo, hi):
    """Widen a 32-bit kernel address to the real VA space.

    Logs print kernel pcs as their low 32 bits (0x1ca2206c); the trace stores the
    full run VA (0xfffffe001ca2206c). Comparing them directly silently reports 0
    hits, which looks exactly like "this function never ran" -- the same trap as
    passing a link VA to HV_PCHK.
    """
    if hi <= 0xffffffff:
        return 0xfffffe0000000000 | lo, 0xfffffe0000000000 | hi
    return lo, hi


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--range", action="append", default=[],
                    metavar="LO:HI", help="count blocks in [LO,HI); repeatable")
    ap.add_argument("--slide", type=lambda x: int(x, 0), default=0,
                    help="added to each --range bound; use for link VAs")
    ap.add_argument("--top", type=int, default=15, metavar="N",
                    help="show the N hottest 4 KiB windows inside each range")
    ap.add_argument("--first", type=int, default=5, metavar="N",
                    help="also show the first N hits with context")
    ap.add_argument("--context", type=int, default=8)
    ap.add_argument("--pattern", action="append", default=[],
                    metavar="LO:HI", help="count blocks in a plain VA window (no slide)")
    ap.add_argument("--last", type=int, default=1,
                    help="show the last N blocks of the whole trace")
    a = ap.parse_args()

    size = os.path.getsize(a.file)
    n = size // 8
    print("trace: %s  %d blocks (%.1f MB)" % (a.file, n, size / 1e6))

    f = open(a.file, "rb")
    mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)

    def blk(i):
        return struct.unpack_from("<Q", mm, i * 8)[0]

    ranges = [(lo + a.slide, hi + a.slide)
              for lo, hi in (fit_kernel(*parse_range(r)) for r in a.range)]
    for lo, hi in ranges:
        hits = []
        for i in range(n):
            v = blk(i)
            if lo <= v < hi:
                hits.append(i)
        print("\nrange [0x%x, 0x%x): %d/%d blocks (%.3f%%)"
              % (lo, hi, len(hits), n, 100.0 * len(hits) / max(n, 1)))
        if not hits:
            continue
        print("  first hit at block %d, last at %d" % (hits[0], hits[-1]))
        c = collections.Counter(blk(i) & ~0xfff for i in hits)
        for win, cnt in c.most_common(a.top):
            print("    0x%-13x %6d" % (win, cnt))
        ctx = []
        for k in range(a.context):
            i = hits[0] - a.context + k
            ctx.append("0x%x" % blk(i) if 0 <= i < n else "-")
        print("  before first hit: %s" % " ".join(ctx))
        ctx = []
        for k in range(a.context):
            i = hits[-1] + 1 + k
            ctx.append("0x%x" % blk(i) if 0 <= i < n else "-")
        print("  after last hit  : %s" % " ".join(ctx))
        if a.first:
            for j, i in enumerate(hits[:a.first]):
                print("  hit[%d] at block %d: %s" % (j, i, blk(i)))

    for lo, hi in (fit_kernel(*parse_range(x)) for x in a.pattern):
        cnt = sum(1 for i in range(n) if lo <= blk(i) < hi)
        print("\nwindow [0x%x, 0x%x): %d blocks" % (lo, hi, cnt))

    # Segments the engine itself distinguishes (same bounds as hv_seg_note).
    segs = [("kext  ", 0xfffffe001bb20000, 0xfffffe001c210000),
            ("kernel", 0xfffffe001c890000, 0xfffffe001eecc000)]
    if a.slide:
        segs = [(nm, lo + a.slide, hi + a.slide) for nm, lo, hi in segs]
    if a.slide:
        print("\n(segment bounds shifted by slide 0x%x)" % a.slide)
    tot = collections.Counter()
    for i in range(n):
        v = blk(i)
        tag = "user/other"
        for nm, lo, hi in segs:
            if lo <= v < hi:
                tag = nm.strip()
                break
        tot[tag] += 1
    print("segments: " + ", ".join("%s %d" % (k, v) for k, v in tot.most_common()))

    if a.last:
        print("\nlast %d blocks:" % a.last)
        for i in range(max(0, n - a.last), n):
            print("  [%d] 0x%x" % (i, blk(i)))
    f.close()


if __name__ == "__main__":
    sys.exit(main())
