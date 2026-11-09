#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Stateless live round reporter for the 7x24 SimNow run.

The CTP logs carry a wall clock (HH:MM:SS) but no date, and the strategy runs
through midnight, so "the last 2 hours" cannot be expressed as a simple
timestamp comparison.  This tool therefore rebuilds a *relative* timeline: it
walks the log backwards and every time the clock jumps forward by more than
12 h it assumes a midnight rollover and shifts the earlier lines back by one
day.  That turns the whole file into a monotonically increasing offset timeline
anchored on today, which makes --hours N work across midnight and across the
several days a log file can span.

Usage
-----
    python tools/rounds724.py --hours 2
    python tools/rounds724.py --hours 6 --instr ag2612
    python tools/rounds724.py --since 13:30:00          # legacy, today only
    python tools/rounds724.py --hours 2 --after-line 507967 --after-query-line 23096

Output is a single JSON line: trips, wins/losses, gross ticks, gross money,
average ticks per trip, win rate and the balance delta reported by query.log.
"""
import argparse
import io
import json
import os
import re
import sys
from datetime import datetime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGDIR = os.path.join(ROOT, "log")

TS_RE = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.\d+\]")
RT_RE = re.compile(
    r"\[(\d\d):(\d\d):(\d\d)\.\d+\].*ROUND TRIP \S+ closed @ ([\d.]+) "
    r"\(entry=([\d.]+) (long|short), pnl=([+-][\d.]+)/unit")
BAL_RE = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.\d+\].*snapshot published: "
                    r"positions=(-?\d+) instruments=\d+ balance=([\d.]+)")

DAY = 86400
REWIND_GUARD = 12 * 3600   # a jump this big means the clock wrapped at midnight


def to_sec(h, m, s):
    return int(h) * 3600 + int(m) * 60 + int(s)


def read_numbered(path):
    """[(lineno, wall_sec, text)] for every line that starts with a timestamp."""
    out = []
    if not os.path.exists(path):
        return out
    with io.open(path, "rb") as f:
        for lineno, raw in enumerate(f, 1):
            text = raw.decode("utf-8", "replace")
            m = TS_RE.match(text)
            if m:
                out.append((lineno, to_sec(m.group(1), m.group(2), m.group(3)), text))
    return out


def relative_timeline(entries):
    """Absolute offsets anchored on today, rebuilt backwards across midnight."""
    stamps = [0] * len(entries)
    offset = 0
    nxt = None
    for i in range(len(entries) - 1, -1, -1):
        sec = entries[i][1]
        if nxt is not None and sec > nxt + REWIND_GUARD:
            offset += DAY          # this line belongs to the previous day
        stamps[i] = sec - offset
        nxt = sec
    return stamps


def count_lines(path):
    if not os.path.exists(path):
        return 0
    with io.open(path, "rb") as f:
        return sum(1 for _ in f)


def collect(path, want_balance, threshold, now_sec, after_line, instr):
    entries = read_numbered(path)
    stamps = relative_timeline(entries)
    rows = []
    for (lineno, _wall, text), abs_sec in zip(entries, stamps):
        if lineno <= after_line:
            continue
        if abs_sec < threshold or abs_sec > now_sec + 3600:
            continue
        if want_balance:
            mb = BAL_RE.search(text)
            if mb:
                rows.append((abs_sec, float(mb.group(5))))
        else:
            m = RT_RE.search(text)
            if not m:
                continue
            if instr and instr not in text:
                continue
            entry = float(m.group(5))
            if entry <= 0:      # phantom-adopted entry: pnl is not trustworthy
                rows.append((abs_sec, None))
                continue
            rows.append((abs_sec, float(m.group(7))))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--since", help="HH:MM:SS wall clock start (today only)")
    ap.add_argument("--hours", type=float,
                    help="window length in hours, counted back from now "
                         "(default 2 when --since is absent)")
    ap.add_argument("--mult", type=float, default=15.0,
                    help="contract multiplier for the money view "
                         "(ag=15 yuan/kg per lot, rb=10)")
    ap.add_argument("--after-line", type=int, default=0,
                    help="only count ROUND TRIP lines after this 1-based line "
                         "number of strategy.log")
    ap.add_argument("--instr", default="",
                    help="only count round trips of this instrument, e.g. ag2612")
    ap.add_argument("--after-query-line", type=int, default=0,
                    help="same as --after-line but for query.log balance lines")
    args = ap.parse_args()

    now = datetime.now()
    now_sec = now.hour * 3600 + now.minute * 60 + now.second

    if args.since:
        threshold = to_sec(*args.since.split(":"))
        window = f"since {args.since}"
    else:
        hours = args.hours if args.hours is not None else 2.0
        threshold = now_sec - int(hours * 3600)
        window = f"last {hours:g}h"

    trips = []
    skipped = 0
    for _abs, pnl in collect(os.path.join(LOGDIR, "strategy.log"), False,
                             threshold, now_sec, args.after_line, args.instr):
        if pnl is None:
            skipped += 1
        else:
            trips.append(pnl)

    bals = [v for _abs, v in collect(os.path.join(LOGDIR, "query.log"), True,
                                     threshold, now_sec, args.after_query_line, "")]

    wins = sum(1 for x in trips if x > 0)
    gross = sum(trips)
    result = {
        "window": window,
        "generated_at": now.strftime("%Y-%m-%d %H:%M:%S"),
        "after_line": args.after_line,
        "strategy_lines": count_lines(os.path.join(LOGDIR, "strategy.log")),
        "trips": len(trips),
        "wins": wins,
        "losses": len(trips) - wins,
        "skipped_phantom": skipped,
        "gross_ticks": round(gross, 2),
        "multiplier": args.mult,
        "gross_money": round(gross * args.mult, 2),
        "avg_ticks": round(gross / len(trips), 2) if trips else None,
        "win_rate": round(100.0 * wins / len(trips), 1) if trips else None,
        "pnl_per_unit": [round(x, 1) for x in trips],
        "balance_delta": round(bals[-1] - bals[0], 2) if len(bals) >= 2 else None,
    }
    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
