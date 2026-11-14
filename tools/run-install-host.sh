#!/usr/bin/env bash
#
# Runs the host installer and waits for the process, not for the clock, retrying until the run
# reaches the kernel fault (or the tries run out).
#
#   tools/run-install-host.sh [log] [seconds] [tries]
#
# Not every run gets as far as the kernel: the restore has its own timeouts, and a run that
# trips one of them ends in DFU before any guest code that matters has run. The retry is there
# because the thing being read is the kernel fault, and a log without it is not a result.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
log=${1:-/tmp/hi$(date +%H%M%S).log}
limit=${2:-180}
tries=${3:-4}
cd "$ROOT" || exit 1

run_once() {
    rm -f work/disk.img work/aux.img
    env CURL_CA_BUNDLE="$ROOT/work/ca-bundle.pem" \
        "$ROOT/build/host/host-install" "$ROOT/work/AVPBooter.vmapple2.bin" \
        "$ROOT/android/ipsw/24G90.ipsw" "$ROOT/work" 4096 2 2>&1 |
        grep -a --line-buffered -E "el1 data abort|hengevm:  |hengevm: ptw |hengevm: logpc |hengevm: daif |hengevm: alive |hengevm: interrupts |hengevm: block|executed from|exception type|^step|restore (failed|finished)|Error" > "$log" &
    pid=$!
    start=$(date +%s)
    while kill -0 "$pid" 2>/dev/null; do
        if [ $(( $(date +%s) - start )) -ge "$limit" ]; then
            kill -TERM "$pid" 2>/dev/null
            break
        fi
        sleep 2
    done
    wait "$pid" 2>/dev/null
    echo "$(( $(date +%s) - start ))s"
}

for n in $(seq 1 "$tries"); do
    secs=$(run_once)
    if grep -qa "el1 data abort" "$log"; then
        echo "run $n: fault reached in ${secs}, log $log ($(wc -c < "$log") bytes)"
        exit 0
    fi
    echo "run $n: no kernel fault in ${secs} (retrying)"
done
echo "no run reached the fault; last log $log"
exit 1
