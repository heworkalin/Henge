#!/usr/bin/env bash
#
# Runs the host installer and waits for the process, not for the clock.
#
#   tools/run-install-host.sh [log] [seconds]
#
# A restore run ends in one of two ways: the installer exits on its own, or it is
# still running when the guest stops answering. Polling every two seconds turns the
# first case into a result at once instead of after the whole wait, and the second
# case still reads the log, which is where the answer is either way.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
log=${1:-/tmp/hi$(date +%H%M%S).log}
limit=${2:-180}
cd "$ROOT" || exit 1
rm -f work/disk.img work/aux.img

env CURL_CA_BUNDLE="$ROOT/work/ca-bundle.pem" \
    ${HV_TRACE:+HV_TRACE=$HV_TRACE} ${HV_LOG:+HV_LOG=$HV_LOG} \
    "$ROOT/build/host/host-install" "$ROOT/work/AVPBooter.vmapple2.bin" \
    "$ROOT/android/ipsw/24G90.ipsw" "$ROOT/work" 4096 2 2>&1 |
    grep -a --line-buffered -E "el1 data abort|hengevm:  |hengevm: block|executed from|exception type|^step|restore (failed|finished)|Error" > "$log" &
pid=$!
start=$(date +%s)
while kill -0 "$pid" 2>/dev/null; do
    now=$(date +%s)
    if [ $((now - start)) -ge "$limit" ]; then
        echo "still running after ${limit}s — reading the log anyway"
        kill -TERM "$pid" 2>/dev/null
        break
    fi
    sleep 2
done
wait "$pid" 2>/dev/null
echo "elapsed $(( $(date +%s) - start ))s, log $log ($(wc -c < "$log" 2>/dev/null) bytes)"
