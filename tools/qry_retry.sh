#!/bin/sh
# Retry the one-shot CTP query until SimNow's front comes back.
# SimNow closes 30001/40001 around 15:00-16:00 for the daily settlement, so a
# query issued at 15:50 simply cannot connect. Nothing here is destructive:
# each attempt is a fresh read-only session that exits on failure.
set -u
cd "$(dirname "$0")/.." || exit 1
export PATH="$PWD/.toolchain/shim:$PATH"
OUT=log/qry_auto
mkdir -p "$OUT"

i=0
while [ "$i" -lt 40 ]; do
    i=$((i + 1))
    for cfg in config/config_simnow.ini config/config_simnow724.ini; do
        tag=$(basename "$cfg" .ini)
        if timeout 150 ./build/bin_simnow/qry_today.exe --config "$cfg" --out "$OUT/$tag" \
                > "$OUT/$tag.run.log" 2>&1; then
            echo "OK $cfg $(date +%H:%M:%S)" >> "$OUT/status.log"
            exit 0
        fi
        if grep -q "## 成交" "$OUT/$tag.run.log" 2>/dev/null; then
            echo "PARTIAL $cfg $(date +%H:%M:%S)" >> "$OUT/status.log"
        else
            echo "FAIL $cfg $(date +%H:%M:%S)" >> "$OUT/status.log"
        fi
    done
    sleep 90
done
echo "GAVEUP $(date +%H:%M:%S)" >> "$OUT/status.log"
