#!/usr/bin/env python3
# Dump the running kernelcache image out of guest RAM, for tools/lcfs.py / ksym.py.
#
#   HENGE_KC_PA=0x73b18000         kimage_pa (default: 24G90's)
#   HENGE_KC_LEN=0x3c00000         bytes to copy (default 60 MiB; 0x4800000 covers kext __LINKEDIT)
#   HENGE_OUT=/data/local/tmp/henge/kc.bin
#   gdb -p <pid> -batch -x tools/henge-gdb/kcdump.py
#
# The image in RAM is the *running* one (slide applied), so symbol offsets line
# up with the log while the on-disk file needs a rebase -- that is the point.

exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

PA = arg("HENGE_KC_PA", 0x73b18000)
LEN = arg("HENGE_KC_LEN", 0x3C00000)
OUT = arg("HENGE_OUT", "/data/local/tmp/henge/kc.bin", str)

out = bytearray()
for off in range(0, LEN, 0x1000):
    b = try_rd_pa(PA + off, 0x1000)
    if b is None:
        print("stop at +0x%x: PA 0x%x not in RAM" % (off, PA + off))
        break
    out += b
with open(OUT, "wb") as f:
    f.write(out)
print("wrote %d bytes (0x%x) from PA 0x%x to %s" % (len(out), len(out), PA, OUT))
