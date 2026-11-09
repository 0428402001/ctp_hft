#!/usr/bin/env python
"""Append the current broker account snapshot to log/account.csv.

Why this exists: on 2026-11-06 the account was queried and it read
pre_balance == balance == 20 997 089.07 with close_profit 0, while everything in
query.log over the previous days said ~999 000 - i.e. the numbers no longer lined
up with our own trade history and there was no way to say what last night cost.
One line per run makes that impossible to lose again.

Usage:  python tools/acct_log.py [--print]
"""
from __future__ import annotations

import csv
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "bin_simnow" / "account_peek.exe"
CSV_PATH = ROOT / "log" / "account.csv"
FIELDS = ["time", "trading_day", "pre_balance", "balance", "available",
          "curr_margin", "close_profit", "position_pnl", "commission",
          "net_today", "positions"]


def read_account() -> dict[str, str] | None:
    if not EXE.exists():
        return None
    try:
        out = subprocess.run([str(EXE)], cwd=str(ROOT), capture_output=True,
                             timeout=20, creationflags=0x08000000)
    except Exception:
        return None
    txt = out.stdout.decode("utf-8", "replace")
    if out.returncode != 0 or not txt.strip():
        return None

    row: dict[str, str] = {}
    for line in txt.splitlines():
        if ":" not in line:
            continue
        key, _, val = line.partition(":")
        key = key.strip()
        val = val.split("<-")[0].split("(")[0].strip()
        row[key] = val
    return {
        "time": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "trading_day": row.get("trading_day", ""),
        "pre_balance": row.get("pre_balance", ""),
        "balance": row.get("balance", ""),
        "available": row.get("available", ""),
        "curr_margin": row.get("curr_margin", ""),
        "close_profit": row.get("close_profit", ""),
        "position_pnl": row.get("position_pnl", ""),
        "commission": row.get("commission", ""),
        "net_today": row.get("net_today", ""),
        "positions": row.get("positions", ""),
    }


def main() -> int:
    row = read_account()
    if row is None:
        print("account_peek unavailable (is query_proc running?)")
        return 1

    new = not CSV_PATH.exists()
    with open(CSV_PATH, "a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        if new:
            w.writeheader()
        w.writerow(row)

    if "--print" in sys.argv:
        for k in FIELDS:
            print(f"{k:13s}: {row[k]}")
    else:
        print(f"{row['time']}  day={row['trading_day']}  "
              f"balance={row['balance']}  net_today={row['net_today']}  "
              f"close={row['close_profit']}  fee={row['commission']}  "
              f"pos={row['positions']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
