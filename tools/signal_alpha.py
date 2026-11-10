"""Is the OBI-extreme signal itself profitable, before any tp/sl is applied?

Every optimisation round so far tuned tp/sl/threshold on top of an unverified
assumption: that an order-book imbalance extreme is followed by a reversal big
enough to pay the round-trip cost. This script tests that assumption directly.

Method
------
For every tick whose |OBI| crosses the threshold (de-duplicated by a cooldown),
measure the mid-price move over the next h ticks, signed in the direction the
strategy would trade:

    OBI <= -th  ->  strategy goes LONG   ->  edge = mid(t+h) - mid(t)
    OBI >= +th  ->  strategy goes SHORT  ->  edge = mid(t) - mid(t+h)

Aggregate mean / median / hit-rate per horizon. No tp, no sl, no path
dependency -- this is the raw predictive power of the signal.

Then compare it against the cost line, which is what actually decides whether
the strategy can work:

    taker entry + taker exit : 1.0 tick cross each side
    fee                      : ~0.64 tick per side (ag: 9.6 CNY/lot, 15 CNY/tick)
    --------------------------------------------------------------
    total                    : ~2.3 ticks per round trip

If the mean edge at any horizon stays below ~2.3 ticks, no combination of tp/sl
can rescue it, and the fix has to be structural (maker entry, longer hold,
better filter), not parametric.

Also reported: the same signal traded in the OPPOSITE direction. If momentum
wins over reversal, the strategy's core premise is wrong for this instrument.

Usage:
    python tools/signal_alpha.py
    python tools/signal_alpha.py --instr rb2701
    python tools/signal_alpha.py --instr ag2612 --cost
"""
import argparse
import re
import statistics

LOG = "log/strategy.log"
HORIZONS = (1, 3, 5, 10, 20, 40, 60, 120)
COOLDOWN = 20          # ticks between two signals of the same kind
MAX_GAP = 60.0         # a sample is dropped if t..t+h spans a session break

TICK_SIZE = {"ag": 1.0, "rb": 1.0, "cu": 10.0, "ni": 10.0, "au": 0.02}
FEE_TICKS = {"ag": 0.64, "rb": 0.20, "cu": 0.55, "ni": 0.60, "au": 0.45}

_TS = rb"\[(\d\d):(\d\d):(\d\d)\.(\d+)\]"
_TAIL = (rb"last=([\d.]+) bid=([\d.]+) ask=([\d.]+) bv=(\d+) av=(\d+) "
         rb"obi=([+-][\d.]+) voi=([+-][\d.]+)")


def load(instr, path=LOG):
    pat = re.compile(_TS + rb".*tickdat " + instr.encode() + rb" " + _TAIL)
    ts, bid, ask, obi, voi = [], [], [], [], []
    day, prev = 0, None
    with open(path, "rb") as f:
        for line in f:
            m = pat.search(line)
            if not m:
                continue
            h, mi, s, frac, _l, b, a, _bv, _av, o, v = m.groups()
            t = (int(h) * 3600 + int(mi) * 60 + int(s)
                 + int(frac.ljust(3, b"0")[:3]) / 1000.0)
            if prev is not None and t < prev - 3600:
                day += 86400
            prev = t
            ts.append(t + day)
            bid.append(float(b))
            ask.append(float(a))
            obi.append(float(o))
            voi.append(float(v))
    return ts, bid, ask, obi, voi


def sec_of_day(t):
    s = t % 86400
    return s + 86400 if s < 9 * 3600 else s


def signals(obi, th, cooldown=COOLDOWN):
    """(index, side) with side=+1 long / -1 short, one per cooldown window."""
    out = []
    nxt = 0
    for i, o in enumerate(obi):
        if i < nxt:
            continue
        if o <= -th:
            out.append((i, 1))
            nxt = i + cooldown
        elif o >= th:
            out.append((i, -1))
            nxt = i + cooldown
    return out


def measure(ts, bid, ask, sig, horizons, scale, invert=False):
    mid = [(b + a) * 0.5 for b, a in zip(bid, ask)]
    N = len(mid)
    stats = {h: [] for h in horizons}
    for i, side in sig:
        if invert:
            side = -side
        for h in horizons:
            j = i + h
            if j >= N or ts[j] - ts[i] > MAX_GAP:
                continue
            stats[h].append((mid[j] - mid[i]) * side / scale)
    return {h: v for h, v in stats.items() if v}


def fmt(vals):
    if not vals:
        return "     -      "
    mean = statistics.fmean(vals)
    hit = sum(1 for x in vals if x > 0) / len(vals) * 100
    return "%+6.2f/%2.0f%%" % (mean, hit)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--instr", default="ag2612")
    ap.add_argument("--log", default=LOG)
    ap.add_argument("--cost", action="store_true",
                    help="print the net-of-cost column at h=20/40/60")
    a = ap.parse_args()

    root = re.match(r"[a-z]+", a.instr)
    scale = TICK_SIZE.get(root.group(0) if root else "", 1.0)
    fee = FEE_TICKS.get(root.group(0) if root else "", 0.6)

    ts, bid, ask, obi, voi = load(a.instr, a.log)
    if not ts:
        print("no ticks for %s in %s" % (a.instr, a.log))
        return
    print("%s : %d ticks, %.1f h of tape (tick = %.3g price units)"
          % (a.instr, len(ts), (ts[-1] - ts[0]) / 3600.0, scale))
    print("cost per round trip: taker 1.0t x2 + fee %.2ft x2 = %.2f ticks\n"
          % (fee, 2.0 + 2 * fee))

    head = "  th   n    " + "".join("%11s" % ("h=%d" % h) for h in HORIZONS)
    print("=== reversal (what the strategy trades): mean edge / hit rate ===")
    print(head + "   [edge in ticks]")
    cache = {}
    for th in (0.55, 0.65, 0.75, 0.85, 0.90, 0.95):
        sig = signals(obi, th)
        cache[th] = sig
        st = measure(ts, bid, ask, sig, HORIZONS, scale)
        print("%.2f %5d  " % (th, len(sig))
              + "".join("%11s" % fmt(st.get(h, [])) for h in HORIZONS))

    print("\n=== momentum (same signal, traded the other way) ===")
    print(head)
    for th in (0.55, 0.75, 0.90):
        st = measure(ts, bid, ask, cache[th], HORIZONS, scale, invert=True)
        print("%.2f %5d  " % (th, len(cache[th]))
              + "".join("%11s" % fmt(st.get(h, [])) for h in HORIZONS))

    if a.cost:
        print("\n=== net of cost at th=0.90 (ticks per round trip) ===")
        st = measure(ts, bid, ask, cache[0.90], HORIZONS, scale)
        for h in (5, 10, 20, 40, 60):
            v = st.get(h, [])
            if not v:
                continue
            gross = statistics.fmean(v)
            print("  h=%-4d gross %+6.2f  cost %5.2f  net %+6.2f   (%.0f%% of samples "
                  "clear cost)" % (h, gross, 2.0 + 2 * fee, gross - (2.0 + 2 * fee),
                                   100.0 * sum(1 for x in v if x > 2.0 + 2 * fee) / len(v)))

    # ---- split by session and by volatility regime at th=0.90 ----
    sig = cache[0.90]
    print("\n=== th=0.90 split by session (h=20 / h=40) ===")
    buckets = {"night 21:00-02:30": lambda s: s >= 21 * 3600 or s < 2 * 3600 + 1800,
               "day   09:00-15:00": lambda s: 9 * 3600 <= s < 15 * 3600}
    for name, pred in buckets.items():
        sub = [(i, sd) for i, sd in sig if pred(sec_of_day(ts[i]))]
        st = measure(ts, bid, ask, sub, (20, 40), scale)
        print("  %-20s n=%-5d h20 %s  h40 %s"
              % (name, len(sub), fmt(st.get(20, [])), fmt(st.get(40, []))))

    print("\n=== th=0.90 split by realised volatility (h=20) ===")
    mid = [(b + a) * 0.5 for b, a in zip(bid, ask)]
    vs = []
    for i, _sd in sig:
        j = i + 20
        if j < len(mid) and ts[j] - ts[i] <= MAX_GAP:
            vs.append(abs(mid[j] - mid[i]) / scale)
    if vs:
        vs.sort()
        p33, p66 = vs[len(vs) // 3], vs[2 * len(vs) // 3]
        print("  vol terciles at h=20: low<%.1ft  mid  high>%.1ft" % (p33, p66))
        for label, lo, hi in (("low ", 0, p33), ("mid ", p33, p66), ("high", p66, 1e9)):
            sub = []
            for i, sd in sig:
                j = i + 20
                if j < len(mid) and ts[j] - ts[i] <= MAX_GAP:
                    v = abs(mid[j] - mid[i]) / scale
                    if lo <= v < hi:
                        sub.append((i, sd))
            st = measure(ts, bid, ask, sub, (20,), scale)
            print("  %s n=%-5d h20 %s" % (label, len(sub), fmt(st.get(20, []))))


if __name__ == "__main__":
    main()
