#!/bin/sh
# Watch both SimNow counters. The day counter (30001) is the only place
# 11-09's fills can live; the 7x24 counter is polled once a minute in case
# its calendar rolls over.
set -u
cd "$(dirname "$0")/.." || exit 1
export PATH="$PWD/.toolchain/shim:$PATH"
LOG=log/qry_watch.log
: > "$LOG"
i=0
while [ $i -lt 120 ]; do
    i=$((i + 1))
    ts=$(date +%H:%M:%S)
    for env in day:config/config_simnow.ini:log/qry_day 724:config/config_simnow724.ini:log/qry_724b; do
        name=${env%%:*}; rest=${env#*:}; cfg=${rest%%:*}; out=${rest#*:}
        # 724: only every 4th tick
        if [ "$name" = "724" ] && [ $((i % 4)) -ne 0 ]; then continue; fi
        if timeout 60 ./build/bin_simnow/qry_today.exe --config "$cfg" --out "$out" > "log/qry_${name}.out" 2>&1; then
            td=$(grep -ao "trading day [0-9]*" "log/qry_${name}.out" | head -1 | awk '{print $3}')
            n=$(grep -ac "成交" "log/qry_${name}.out")
            echo "[$ts] $name OK tradingday=$td" >> "$LOG"
            cp "log/qry_${name}.out" "log/qry_${name}.ok.out"
        else
            echo "[$ts] $name fail :: $(grep -aE 'login failed|connect timeout' "log/qry_${name}.out" | head -1)" >> "$LOG"
        fi
    done
    sleep 10
done
