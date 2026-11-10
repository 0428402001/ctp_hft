"""Sweep entry filters x bracket exits on the recorded SimNow ticks, net of cost.

Why another backtester next to bt_bracket.py: that one fixed tp/sl and ignored
the two things that decide whether this strategy can pay for itself -- the
overshoot ("price already ran too far") filter, and the round-trip cost
(taker entry + fee). Every combination it tested came out negative, which is
the expected answer when cost is left out of the entry decision.

Cost model, in ticks of the instrument (ag2612: 1 tick = 1 CNY/kg = 15 CNY/lot):
  entry  : taker, crosses 1 tick from the touch            -> 1.0
  TP exit: resting limit at entry +/- tp, filled on touch   -> 0.0
  SL exit: IOC, slips --sl-slip ticks past the trigger      -> 1.5 on stops
  fee    : --fee per side (default 1.0, i.e. ~15 CNY/lot/side)

No numpy on this box (pip is not reachable), so everything is plain lists.

Usage:
    python tools/bt_sweep.py
    python tools/bt_sweep.py --fee 0.6 --max-spread 2 --min-trades 40
"""
import argparse
import re

LOG = "log/strategy.log"
INSTR = "ag2612"
STALL = 0        # ticks the price must have come off the recent extreme
STALL_WIN = 6    # look-back for that extreme
BE = 0           # breakeven stop after N ticks of profit

TICKDAT = re.compile(
    rb"\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*tickdat " + INSTR.encode() + rb" "
    rb"last=([\d.]+) bid=([\d.]+) ask=([\d.]+) bv=(\d+) av=(\d+) "
    rb"obi=([+-][\d.]+) voi=([+-][\d.]+)"
)

# No trading in the first minutes of a session, nor into the close.
BLACKOUT = ((9 * 3600, 9 * 3600 + 180), (21 * 3600, 21 * 3600 + 120),
            (13 * 3600 + 1800, 13 * 3600 + 1860), (14 * 3600 + 3300, 15 * 3600))


def load(path=LOG):
    ts, bid, ask, obi, voi = [], [], [], [], []
    day, prev = 0, None
    with open(path, "rb") as f:
        for line in f:
            m = TICKDAT.search(line)
            if not m:
                continue
            h, mi, s, frac, _last, b, a, _bv, _av, o, v = m.groups()
            t = int(h) * 3600 + int(mi) * 60 + int(s) + int(frac.ljust(3, b"0")[:3]) / 1000.0
            if prev is not None and t < prev - 3600:  # midnight wrap in a 18 h log
                day += 86400
            prev = t
            ts.append(t + day)
            bid.append(float(b))
            ask.append(float(a))
            obi.append(float(o))
            voi.append(float(v))
    return ts, bid, ask, obi, voi


def mid_drift(ts, mid, window):
    """mid(t) - mid(t-window); 0 while the window is less than 60% covered."""
    out = [0.0] * len(ts)
    j = 0
    for i in range(len(ts)):
        target = ts[i] - window
        while j + 1 < i and ts[j + 1] <= target:
            j += 1
        if ts[i] - ts[j] >= window * 0.6:
            out[i] = mid[i] - mid[j]
    return out


def sec_of_day(t):
    """Seconds since midnight, with the night session shifted past 24 h so a
    single comparison window works (21:00-02:30 is one continuous session)."""
    s = t % 86400
    return s + 86400 if s < 9 * 3600 else s


SESSION = None  # (lo, hi) in sec_of_day space, or None = both


def entries_of(ts, bid, ask, obi, voi, drift, obi_th, voi_th, trend, max_spread):
    out = []
    for i in range(len(ts) - 2):
        if ask[i] - bid[i] > max_spread:
            continue
        sec = ts[i] % 86400
        if any(a <= sec < b for a, b in BLACKOUT):
            continue
        if SESSION and not (SESSION[0] <= sec_of_day(ts[i]) < SESSION[1]):
            continue
        side = 0
        if obi[i] <= -obi_th and voi[i] >= voi_th and drift[i] <= -trend:
            side = 1
        elif obi[i] >= obi_th and voi[i] <= -voi_th and drift[i] >= trend:
            side = -1
        if not side:
            continue
        # --- stall filter: price must have come OFF the extreme already -------
        # Fading a book imbalance is only an edge once the impulse has stopped
        # printing new extremes; entering while it is still extending is what
        # produced every <=10 s stop-out in the 2026-11-10 day session.
        if STALL > 0:
            lo = max(0, i - STALL_WIN + 1)
            if side > 0:
                if bid[i] - min(bid[lo:i + 1]) < STALL:
                    continue
            else:
                if max(ask[lo:i + 1]) - ask[i] < STALL:
                    continue
        out.append((i, side))
    return out


def simulate(entries, bid, ask, tp, sl, hold, a):
    n = wins = losses = timeouts = 0
    pnl = 0.0
    hold_sum = 0
    last_exit = -10 ** 9
    N = len(bid)
    for i, side in entries:
        if i <= last_exit + a.cooldown or i + 1 >= N:
            continue
        e = i + 1
        entry = (ask[e] + a.slip) if side > 0 else (bid[e] - a.slip)
        tp_px = entry + side * tp
        sl_px = entry - side * sl
        stop_px = sl_px
        j = e + 1
        end = min(N, e + 1 + hold)
        exit_px = None
        while j < end:
            if side > 0:
                if bid[j] >= tp_px:
                    exit_px, wins = tp_px, wins + 1
                    break
                # breakeven: once BE ticks of profit printed, the stop moves to
                # the entry price and the trip can no longer lose (minus fees).
                if BE > 0 and bid[j] - entry >= BE:
                    stop_px = max(stop_px, entry)
                if bid[j] <= stop_px:
                    exit_px = bid[j] - (a.sl_slip if stop_px != entry else 0.0)
                    losses += 1
                    break
            else:
                if ask[j] <= tp_px:
                    exit_px, wins = tp_px, wins + 1
                    break
                if BE > 0 and entry - ask[j] >= BE:
                    stop_px = min(stop_px, entry)
                if ask[j] >= stop_px:
                    exit_px = ask[j] + (a.sl_slip if stop_px != entry else 0.0)
                    losses += 1
                    break
            j += 1
        if exit_px is None:
            if j >= N:
                break
            exit_px = bid[j] if side > 0 else ask[j]
            timeouts += 1
        pnl += (exit_px - entry) * side - 2.0 * a.fee
        hold_sum += j - e
        n += 1
        last_exit = j
    return n, wins, losses, timeouts, pnl, (hold_sum / n if n else 0.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fee", type=float, default=1.0, help="fee per side, ticks")
    ap.add_argument("--slip", type=float, default=1.0, help="entry slippage, ticks")
    ap.add_argument("--sl-slip", type=float, default=1.5, help="stop slippage, ticks")
    ap.add_argument("--max-spread", type=int, default=1)
    ap.add_argument("--cooldown", type=int, default=12)
    ap.add_argument("--min-trades", type=int, default=25)
    ap.add_argument("--trend-window", type=int, default=30)
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--split", action="store_true", help="also print night/day split")
    ap.add_argument("--log", default=LOG, help="strategy.log to replay")
    ap.add_argument("--session", default="both", choices=("both", "night", "day"),
                    help="restrict entries to one session")
    ap.add_argument("--eval", default="",
                    help="evaluate one config: obi,voi,trend,tp,sl,hold "
                         "(e.g. 0.92,0.1,1,8,6,120) instead of the grid")
    ap.add_argument("--stall", type=int, default=0,
                    help="entry filter: price must have come >= N ticks off the "
                         "recent extreme (0 = off)")
    ap.add_argument("--stall-win", type=int, default=6,
                    help="look-back in ticks for the stall filter's extreme")
    ap.add_argument("--be", type=int, default=0,
                    help="breakeven stop after N ticks of profit (0 = off)")
    a = ap.parse_args()

    global SESSION, STALL, STALL_WIN, BE
    STALL, STALL_WIN, BE = a.stall, a.stall_win, a.be
    if a.session == "night":
        SESSION = (21 * 3600, 26.5 * 3600)
    elif a.session == "day":
        SESSION = (9 * 3600, 15 * 3600)
    ts, bid, ask, obi, voi = load(a.log)
    mid = [(b + s) / 2.0 for b, s in zip(bid, ask)]
    drift = mid_drift(ts, mid, a.trend_window)
    spreads = sorted(round(s - b) for b, s in zip(bid, ask))
    print(f"ticks {len(ts)}  span {(ts[-1] - ts[0]) / 3600:.1f} h  "
          f"spread median {spreads[len(spreads) // 2]} tick")

    if a.eval:
        obi_th, voi_th, trend, tp, sl, hold = (float(x) for x in a.eval.split(","))
        ents = entries_of(ts, bid, ask, obi, voi, drift, obi_th, voi_th, trend,
                          a.max_spread)
        print(f"config obi={obi_th} voi={voi_th} trend={trend} tp={tp} sl={sl} "
              f"hold={hold} spread<={a.max_spread} fee={a.fee}")
        n, w, l, t, pnl, hh = simulate(ents, bid, ask, tp, sl, int(hold), a)
        print(f"  signals {len(ents)}  trades {n}  win {w}  loss {l}  timeout {t}")
        print(f"  net {pnl:+.1f} tick  exp {pnl / max(1, n):+.2f} tick/trade  "
              f"avg hold {hh:.0f} ticks")
        if a.split:
            for label, lo, hi in (("night 21:00-02:30", 21 * 3600, 26.5 * 3600),
                                  ("day   09:00-15:00", 9 * 3600, 15 * 3600)):
                sub = [e for e in ents
                       if lo <= (ts[e[0]] % 86400 if ts[e[0]] % 86400 >= lo else ts[e[0]] % 86400 + 86400) < hi]
                n2, w2, l2, t2, p2, h2 = simulate(sub, bid, ask, tp, sl, int(hold), a)
                print(f"  {label}: trades {n2:>4}  win {w2:>3}  loss {l2:>3}  "
                      f"net {p2:+7.1f}  exp {p2 / max(1, n2):+.2f}")
        return

    rows = []
    for obi_th in (0.85, 0.90, 0.92, 0.95):
        for voi_th in (0.10, 0.20, 0.30):
            for trend in (0, 1, 2, 3):
                ents = entries_of(ts, bid, ask, obi, voi, drift, obi_th, voi_th,
                                  trend, a.max_spread)
                if len(ents) < 5:
                    continue
                for tp in (6, 8, 10, 12, 16):
                    for sl in (4, 6, 8):
                        for hold in (120, 300):
                            n, w, l, t, pnl, hh = simulate(ents, bid, ask, tp, sl, hold, a)
                            if n < a.min_trades:
                                continue
                            rows.append((pnl / n, n, w, l, t, pnl, hh,
                                         obi_th, voi_th, trend, tp, sl, hold, len(ents)))
    rows.sort(reverse=True)
    hdr = (f"{'exp':>7} {'n':>4} {'win':>4} {'los':>4} {'to':>3} {'net':>8} "
           f"{'hold':>5} {'sig':>5}  obi  voi tr  tp sl hold")
    print(hdr)
    print("-" * (len(hdr) + 4))
    for r in rows[:a.top]:
        exp, n, w, l, t, pnl, hh, obi_th, voi_th, trend, tp, sl, hold, sig = r
        print(f"{exp:>7.2f} {n:>4} {w:>4} {l:>4} {t:>3} {pnl:>8.1f} {hh:>5.0f} "
              f"{sig:>5}  {obi_th:.2f} {voi_th:.1f} {trend:>2} {tp:>3} {sl:>2} {hold:>4}")
    if rows:
        print(f"\ncombos {len(rows)}, profitable {sum(1 for r in rows if r[0] > 0)}")


if __name__ == "__main__":
    main()
