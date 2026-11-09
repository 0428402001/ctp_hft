#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""One-off afternoon-close diagnostic for 2026-11-05.

Reads log/strategy.log from the Nov-5 day-start line (found via
'strategy initialised' at 08:39:52, line 420175) and segments today's
ROUND TRIP / ESCAPE lines by trading window. The bundled rounds724.py
filters by wall-clock time only (no date), so it mixes in the Nov-4
night session; this script avoids that.
"""
import io
import re
import sys

RT = re.compile(
    r"\[(\d\d):(\d\d):(\d\d)\.\d+\].*ROUND TRIP \S+ closed @ ([\d.]+) "
    r"\(entry=([\d.]+) (long|short), pnl=([+-][\d.]+)/unit, "
    r"(passive|IOC escape)\)")
ESC = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.\d+\].*ESCAPE (\S+): (.*)")

START_LINE = 420175  # Nov-5 08:39:52 init
PATH = r"D:\quant\ctp_hft\log\strategy.log"


def sec(h, m, s):
    return int(h) * 3600 + int(m) * 60 + int(s)


def parse():
    trips, escapes = [], []
    with io.open(PATH, encoding="utf-8", errors="replace") as f:
        for i, ln in enumerate(f, 1):
            if i < START_LINE:
                continue
            m = RT.search(ln)
            if m:
                trips.append(dict(
                    t=sec(m.group(1), m.group(2), m.group(3)),
                    entry=float(m.group(5)),
                    pnl=float(m.group(7)),
                    kind=m.group(8)))
                continue
            e = ESC.search(ln)
            if e:
                escapes.append(dict(
                    t=sec(e.group(1), e.group(2), e.group(3)),
                    msg=e.group(4)))
    return trips, escapes


def seg(arr, lo, hi):
    return [x for x in arr if lo <= x['t'] < hi]


def report(name, arr, escs):
    if not arr:
        print(name + ": no trips")
        return
    wins = [x['pnl'] for x in arr if x['pnl'] > 0]
    losses = [x['pnl'] for x in arr if x['pnl'] < 0]
    zeros = [x for x in arr if x['pnl'] == 0]
    gross = sum(x['pnl'] for x in arr)
    escapes = [x for x in arr if x['kind'] == 'IOC escape']
    passives = [x for x in arr if x['kind'] == 'passive']
    esc_pnls = [x['pnl'] for x in escapes]
    pas_pnls = [x['pnl'] for x in passives]
    avg_esc = sum(esc_pnls) / len(esc_pnls) if esc_pnls else None
    avg_pas = sum(pas_pnls) / len(pas_pnls) if pas_pnls else None
    avg_w = sum(wins) / len(wins) if wins else None
    avg_l = sum(losses) / len(losses) if losses else None
    print("%s: trips=%d W=%d L=%d Z=%d gross=%+.1ft winrate=%.1f%%"
          % (name, len(arr), len(wins), len(losses), len(zeros), gross,
             len(wins) / len(arr) * 100))
    print("   avg_win=%s avg_loss=%s" % (
        ("%+.2ft" % avg_w) if avg_w is not None else "n/a",
        ("%+.2ft" % avg_l) if avg_l is not None else "n/a"))
    print("   IOC_escape n=%d avg=%s | passive n=%d avg=%s (passive_share=%.1f%%)"
          % (len(escapes),
             ("%+.2ft" % avg_esc) if avg_esc is not None else "n/a",
             len(passives),
             ("%+.2ft" % avg_pas) if avg_pas is not None else "n/a",
             len(passives) / len(arr) * 100))
    hold = [e for e in escs if 'hold expired' in e['msg'] or 'without a close' in e['msg']]
    other = len(escs) - len(hold)
    print("   ESCAPE lines=%d (hold_expired=%d other=%d)"
          % (len(escs), len(hold), other))


def main():
    trips, escapes = parse()
    morning, lunch = sec(9, 0, 0), sec(11, 30, 0)
    aft, close = sec(13, 30, 0), sec(15, 1, 0)
    cut = sec(14, 24, 0)

    m, me = seg(trips, morning, lunch), seg(escapes, morning, lunch)
    a, ae = seg(trips, aft, close), seg(escapes, aft, close)
    d, de = seg(trips, morning, close), seg(escapes, morning, close)
    a1, a1e = seg(trips, aft, cut), seg(escapes, aft, cut)
    a2, a2e = seg(trips, cut, close), seg(escapes, cut, close)

    report("MORNING  09:00-11:30", m, me)
    report("AFTERNOON 13:30-15:00", a, ae)
    report("FULLDAY  09:00-15:00", d, de)
    report("PM pre-14:24 (old params)", a1, a1e)
    report("PM post-14:24 (profit1/hold60)", a2, a2e)

    esc_pnls = sorted(x['pnl'] for x in d if x['kind'] == 'IOC escape')
    print("FULLDAY IOC escape pnl dist:", esc_pnls)
    # morning old-logic big losses
    print("MORNING trips pnl:", [x['pnl'] for x in m])
    print("AFTERNOON trips pnl:", [x['pnl'] for x in a])
    return 0


if __name__ == "__main__":
    sys.exit(main())
