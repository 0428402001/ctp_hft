"""Mid-frequency tick alpha: composite microstructure signals at longer horizons.

The single-signal test (signal_alpha.py) showed raw OBI-extreme edges peak at
~0.6t -- below the 1.28t fee floor. This script tests whether COMPOSITE
conditions (OBI extreme + VOI confirmation + volatility regime) and LONGER
horizons (h = 20..480 ticks, i.e. ~10 s .. 4 min on 2 tick/s tape) produce an
edge big enough for a mid-frequency strategy targeting 200+ trades/day.

Direction: momentum (follow the imbalance) -- the measured profitable side.

Usage:
    python tools/alpha_composite.py --instr ag2702
"""
import argparse
import math
import re
import statistics as st

LOG = "log/strategy.log"
HORIZONS = (20, 60, 120, 240, 480)
COOLDOWN = 60           # ticks between signals (30 s at 2 tick/s)

_TS = rb"\[(\d\d):(\d\d):(\d\d)\.(\d+)\]"
_TAIL = (rb"last=([\d.]+) bid=([\d.]+) ask=([\d.]+) bv=(\d+) av=(\d+) "
         rb"obi=([+-][\d.]+) voi=([+-][\d.]+)")


def load(instr, path=LOG):
    rx = re.compile(_TS + rb".*?tickdat " + instr.encode() + rb" " + _TAIL)
    out = []
    with open(path, "rb") as f:
        for ln in f:
            m = rx.search(ln)
            if not m:
                continue
            hh, mm, ss, ms = int(m[1]), int(m[2]), int(m[3]), int(m[4])
            t = hh * 3600 + mm * 60 + ss + ms / 1000.0
            bid, ask = float(m[6]), float(m[7])
            if bid <= 0 or ask <= 0:
                continue
            mid = (bid + ask) / 2.0
            out.append((t, mid, float(m[10]), float(m[11]), ask - bid))
    return out


def rollvol(mids, i, win=200):
    """Realised vol over the past `win` mids, in ticks (std of 1-tick diffs)."""
    if i < win + 1:
        return None
    seg = [mids[j + 1] - mids[j] for j in range(i - win, i)]
    if len(seg) < 10:
        return None
    return st.pstdev(seg)


def measure(rows, name, cond, horizons=HORIZONS):
    """cond(obi, voi, vol) -> direction (+1/-1) or 0."""
    n = len(rows)
    vols = [None] * n
    mids = [r[1] for r in rows]
    for i in range(n):
        v = rollvol(mids, i)
        if v is not None:
            vols[i] = v
    known = sorted(v for v in vols if v is not None)
    if not known:
        return
    v_hi = known[int(len(known) * 0.67)]

    last_sig = -10 ** 9
    hits = {h: [] for h in horizons}
    sig_count = 0
    for i in range(n):
        if i - last_sig < COOLDOWN:
            continue
        t, mid, obi, voi, spread = rows[i]
        v = vols[i]
        d = cond(obi, voi, v, v_hi)
        if d == 0:
            continue
        ok_any = False
        for h in horizons:
            j = i + h
            if j >= n:
                continue
            span = rows[j][0] - t
            if span < 0:
                span += 86400
            if span > h * 2:      # session break or stale tape
                continue
            edge = d * (rows[j][1] - mid)
            hits[h].append(edge)
            ok_any = True
        if ok_any:
            sig_count += 1
            last_sig = i

    hours = (rows[-1][0] - rows[0][0]) / 3600.0
    if hours <= 0:
        hours += 24
    print(f"\n=== {name} : {sig_count} signals ({sig_count / max(hours, 1):.1f}/h) ===")
    print(f"{'h':>5} {'n':>6} {'mean':>7} {'med':>6} {'hit':>5} {'t':>6} {'%>=2t':>6} {'%>=4t':>6}")
    for h in horizons:
        e = hits[h]
        if len(e) < 30:
            print(f"{h:>5} {len(e):>6}  (too few)")
            continue
        mu = st.mean(e)
        se = st.stdev(e) / math.sqrt(len(e))
        print(f"{h:>5} {len(e):>6} {mu:>7.2f} {st.median(e):>6.2f} "
              f"{sum(1 for x in e if x > 0) / len(e):>5.0%} {mu / se:>6.2f} "
              f"{sum(1 for x in e if x >= 2) / len(e):>6.0%} "
              f"{sum(1 for x in e if x >= 4) / len(e):>6.0%}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--instr", default="ag2702")
    ap.add_argument("--log", default=LOG)
    a = ap.parse_args()
    rows = load(a.instr, a.log)
    print(f"{a.instr}: {len(rows)} ticks")

    measure(rows, "S1 momentum |obi|>=0.90",
            lambda o, v, vol, vh: (1 if o >= 0.90 else (-1 if o <= -0.90 else 0)))
    measure(rows, "S2 S1 + voi confirm, |obi|>=0.85",
            lambda o, v, vol, vh: (1 if (o >= 0.85 and v > 0) else
                                   (-1 if (o <= -0.85 and v < 0) else 0)))
    measure(rows, "S3 S2 + high-vol regime",
            lambda o, v, vol, vh: (1 if (o >= 0.85 and v > 0 and vol and vol >= vh) else
                                   (-1 if (o <= -0.85 and v < 0 and vol and vol >= vh) else 0)))
    measure(rows, "S4 reversal |obi|>=0.90 (reference)",
            lambda o, v, vol, vh: (-1 if o >= 0.90 else (1 if o <= -0.90 else 0)))


if __name__ == "__main__":
    main()
