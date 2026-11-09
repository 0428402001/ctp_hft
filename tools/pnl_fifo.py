#!/usr/bin/env python
"""Rebuild realised P&L from raw TRADE lines (FIFO), not from ROUND TRIP lines.

The ROUND TRIP line is written by the strategy and is only as good as its own
bookkeeping: on 2026-11-05 it emitted "entry=0.00 long, pnl=+12370.00/unit"
phantoms, and it has no idea about a position that was never closed.  The TRADE
lines are the fills themselves, so adding them up FIFO gives the number the
broker actually booked (before fees).

    python tools/pnl_fifo.py                 # every instrument, whole log
    python tools/pnl_fifo.py --after 523450  # only fills after this log line
    python tools/pnl_fifo.py --instr ag2612 --fee-ticks 2
"""
from __future__ import annotations

import argparse
import re
from collections import defaultdict

LOG = "log/strategy.log"

TRADE_RE = re.compile(
    r"^\[(?P<t>[\d:.]+)\]\[INF\]\[strategy\] TRADE (?P<instr>\S+) "
    r"(?P<side>BUY|SELL) (?P<off>OPEN|CLOSE) vol=(?P<v>\d+) @ (?P<px>[\d.]+)")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--after", type=int, default=0)
    ap.add_argument("--instr", default="")
    ap.add_argument("--mult", type=float, default=15.0)
    ap.add_argument("--tick", type=float, default=1.0)
    ap.add_argument("--fee-ticks", type=float, default=2.0,
                    help="round-trip fee in ticks per lot (ag ~2)")
    args = ap.parse_args()

    lots: dict[str, list[list]] = defaultdict(list)   # instr -> [[px, signed_vol]]
    realised: dict[str, float] = defaultdict(float)
    volume: dict[str, int] = defaultdict(int)
    first: dict[str, str] = {}
    last: dict[str, str] = {}

    with open(LOG, "r", encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f, 1):
            if i <= args.after:
                continue
            m = TRADE_RE.match(line)
            if not m:
                continue
            instr = m.group("instr")
            if args.instr and instr != args.instr:
                continue
            px = float(m.group("px"))
            v = int(m.group("v"))
            sign = v if m.group("side") == "BUY" else -v
            first.setdefault(instr, m.group("t"))
            last[instr] = m.group("t")
            volume[instr] += v

            book = lots[instr]
            remaining = sign
            while remaining and book:
                head = book[0]
                if (head[0] > 0) == (remaining > 0):
                    break                    # same side: just stacks up
                take = min(abs(remaining), abs(head[0]))
                direction = 1 if remaining < 0 else -1   # closing a long? see sign
                # closing when the new fill is opposite to the open lot
                realised[instr] += (px - head[1]) * take * (1 if head[0] > 0 else -1)
                head[0] += take if remaining > 0 else -take
                remaining += take if remaining < 0 else -take
                if head[0] == 0:
                    book.pop(0)
            if remaining:
                book.append([remaining, px])

    total_gross = 0.0
    print(f"{'instr':<10}{'lots':>6}{'gross_ticks':>13}{'gross_money':>14}"
          f"{'fee_money':>13}{'net_money':>14}   first -> last")
    for instr in sorted(realised.keys() | volume.keys()):
        gross = realised[instr] / args.tick          # price points -> ticks
        fee = volume[instr] * args.fee_ticks / 2.0 * args.mult * args.tick
        money = gross * args.mult * args.tick
        total_gross += money
        print(f"{instr:<10}{volume[instr]:>6}{gross:>13.0f}{money:>14.0f}"
              f"{-fee:>13.0f}{money - fee:>14.0f}   {first[instr]} -> {last[instr]}")

    open_lots = {k: v for k, v in lots.items() if any(x[0] for x in v)}
    if open_lots:
        print("\nstill open at the end of the log:")
        for k, v in open_lots.items():
            for qty, px in v:
                if qty:
                    print(f"  {k}: {qty:+d} @ {px:.2f}")
    print(f"\ngross money (fees excluded): {total_gross:,.0f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
