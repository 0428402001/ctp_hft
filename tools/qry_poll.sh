#!/bin/sh
# Poll the SimNow front until a login succeeds, then dump today's trades.
# The counter only keeps the CURRENT trading day, so the first successful
# login after 16:00 is the last moment 11-09's fills can still be read.
set -u
cd "$(dirname "$0")/.." || exit 1
export PATH="$PWD/.toolchain/shim:$PATH"

CFG=${1:-config/config_simnow724.ini}
OUT=${2:-log/qry_724}
LOG="log/qry_poll.log"
mkdir -p log

i=0
while [ $i -lt 240 ]; do
    i=$((i + 1))
    ts=$(date +%H:%M:%S)
    if ./build/bin_simnow/qry_today.exe --config "$CFG" --out "$OUT" > "log/qry_last.out" 2>&1; then
        echo "[$ts] OK attempt=$i" >> "$LOG"
        cat "log/qry_last.out" >> "$LOG"
        exit 0
    fi
    echo "[$ts] fail attempt=$i :: $(grep -aE 'login failed|connect timeout|rsp error' "log/qry_last.out" | head -1)" >> "$LOG"
    sleep 8
done
echo "[$(date +%H:%M:%S)] gave up after $i attempts" >> "$LOG"
exit 1
