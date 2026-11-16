#!/bin/bash
# Watch a running host-install: sample the guest's EL0/syscall counters until they stop
# moving, then dump the rings that recorded what the guest did.
#
# Meant to be started in the background right after the installer:
#   nohup ./tools/watch-stall.sh > /tmp/watch.out 2>&1 &
#
# A stall is three consecutive samples with identical output, which also means the CPUs
# stayed at EL1 and no new syscall or userland block appeared in between.
DEVICE=${DEVICE:-emulator-5554}
G=${G:-/data/local/tmp/henge/gdb-bundle/bin/gdb}
LOG=${LOG:-/tmp/watch.log}
INTERVAL=${INTERVAL:-4}
MAX=${MAX:-80}
: > "$LOG"

sample() {
    adb -s "$DEVICE" shell "$G -p \$(pidof host-install) -batch \
        -ex \"set print thread-events off\" \
        -ex \"p hv_svc_n\" -ex \"p hv_el0_trace_n\" -ex \"p/x hv_last_el0_va\" \
        -ex \"p g_cpus[0].el\" -ex \"p g_cpus[1].el\" 2>/dev/null" \
        | tr -d '\r' | grep '^\$' | tr '\n' ' '
}

prev=""
stable=0
for i in $(seq 1 "$MAX"); do
    t=$(date "+%H:%M:%S")
    out=$(sample)
    echo "$t  ${out:-<no process>}" >> "$LOG"
    if [ -n "$out" ] && [ "$out" = "$prev" ]; then
        stable=$((stable + 1))
    else
        stable=0
        prev="$out"
    fi
    if [ "$stable" -ge 3 ]; then
        echo "$t  STALL detected (3 identical samples)" >> "$LOG"
        break
    fi
    sleep "$INTERVAL"
done

# The rings first, then the structures.
adb -s "$DEVICE" shell "$G -p \$(pidof host-install) -batch -ex 'set print thread-events off' \
    -x /data/local/tmp/henge/dump-history.py" > /tmp/history.log 2>&1
echo "$(date "+%H:%M:%S")  dump written to /tmp/history.log" >> "$LOG"
