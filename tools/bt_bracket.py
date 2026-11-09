"""Backtest entry-signal filters x bracket exits on recorded SimNow ticks.

Replays tickdat lines from log/strategy.log (they carry the exact OBI/VOI the
live strategy computed), simulates:

  entry : signal rules (grid over thresholds)
  fill  : taker entry at bid1+1 (long) / ask1-1 (short), next tick
  exit  : bracket - resting TP at entry +/- tp_ticks (fills when touch
          crosses it), stop when touch moves sl_ticks against entry
          (fill at touch - extra slippage), timeout after hold ticks
          (fill at touch).

Usage: python tools/bt_bracket.py [--since HH:MM --until HH:MM]
"""
import re
import sys
from collections import deque

LOG = "log/strategy.log"
INSTR = "ag2612"          # instrument to replay (was rb2701)
TICK = 1.0                # price tick of INSTR (ag: 1 CNY/kg, rb: 1 CNY/t)

pat = re.compile(
    rb"\[(\d\d):(\d\d):(\d\d)\.\d+\].*tickdat " + INSTR.encode() + rb" "
    rb"last=([\d.]+) bid=([\d.]+) ask=([\d.]+) bv=(\d+) av=(\d+) "
    rb"obi=([+-][\d.]+) voi=([+-][\d.]+)"
)

SINCE = "00:00"
UNTIL = "23:59"
for i, a in enumerate(sys.argv):
    if a == "--since":
        SINCE = sys.argv[i + 1]
    if a == "--until":
        UNTIL = sys.argv[i + 1]


def load():
    rows = []
    for line in open(LOG, "rb"):
        m = pat.search(line)
        if not m:
            continue
        t = m.group(1).decode() + ":" + m.group(2).decode() + ":" + m.group(3).decode()
        if not (SINCE <= t <= UNTIL):
            continue
        rows.append(
            dict(
                t=t,
                bid=float(m.group(5)),
                ask=float(m.group(6)),
                obi=float(m.group(9)),
                voi=float(m.group(10)),
            )
        )
    return rows


def run(rows, window, obi_th, voi_entry, require_decay, tp, sl, hold_max,
        entry_slip=1, stop_slip=1):
    """Returns (n, wins, losses, ticks, avg_hold)."""
    win = deque(maxlen=window)
    wins = losses = flat = 0
    pnl = 0.0
    holds = []
    i = 0
    n_rows = len(rows)
    cooldown = 0
    while i < n_rows:
        r = rows[i]
        win.append(r["obi"])
        if cooldown > 0:
            cooldown -= 1
            i += 1
            continue
        if len(win) == window:
            mx, mn = max(win), min(win)
            bE = mx > obi_th
            sE = mn < -obi_th
            obi_now = r["obi"]
            decay_b = (not require_decay) or obi_now < obi_th - 0.05
            decay_s = (not require_decay) or obi_now > -obi_th + 0.05
            # signal: buy pressure extreme + flow reversed down (+decay)
            sig_sell = bE and not sE and r["voi"] < -voi_entry and decay_b
            sig_buy = sE and not bE and r["voi"] > voi_entry and decay_s
            if sig_sell or sig_buy:
                direction = -1 if sig_sell else +1
                # entry next tick, taker with 1 tick slip
                if i + 1 < n_rows:
                    nxt = rows[i + 1]
                    if direction > 0:
                        entry = min(nxt["bid"] + entry_slip * TICK, nxt["ask"])
                    else:
                        entry = max(nxt["ask"] - entry_slip * TICK, nxt["bid"])
                    # bracket walk
                    j = i + 2
                    exited = False
                    hold = 0
                    while j < n_rows and hold < hold_max:
                        px = rows[j]
                        hold += 1
                        if direction > 0:
                            touch = px["bid"]
                            if touch >= entry + tp * TICK:
                                pnl += tp
                                wins += 1
                                exited = True
                            elif touch <= entry - sl * TICK:
                                fill = touch - stop_slip * TICK
                                pnl += (fill - entry) / TICK
                                losses += 1
                                exited = True
                        else:
                            touch = px["ask"]
                            if touch <= entry - tp * TICK:
                                pnl += tp
                                wins += 1
                                exited = True
                            elif touch >= entry + sl * TICK:
                                fill = touch + stop_slip * TICK
                                pnl += (entry - fill) / TICK
                                losses += 1
                                exited = True
                        if exited:
                            holds.append(hold)
                            break
                        j += 1
                    if not exited and j < n_rows:
                        px = rows[j]
                        touch = px["bid"] if direction > 0 else px["ask"]
                        fill = touch - (stop_slip * TICK if direction > 0 else 0)
                        pnl += ((fill - entry) if direction > 0
                                else (entry - fill)) / TICK
                        flat += 1
                        holds.append(hold_max)
                    cooldown = 10  # brief cooldown between rounds
        i += 1
    n = wins + losses + flat
    return n, wins, losses, flat, pnl, (sum(holds) / len(holds) if holds else 0)


rows = load()
print(f"ticks loaded: {len(rows)}  ({SINCE}-{UNTIL})")

# grid 1: entry filters with fixed bracket tp=3 sl=4 hold=60 (user spec)
grid = []
for obi_th in (0.8, 0.85, 0.9):
    for voi_entry in (0.0, 0.1, 0.2):
        for require_decay in (False, True):
            n, w, l, f, p, h = run(rows, 12, obi_th, voi_entry, require_decay, 3, 4, 60)
            if n:
                grid.append((p / n, n, w, l, f, p, h, obi_th, voi_entry, require_decay))
grid.sort(reverse=True)
print(f"{'exp':>6} {'n':>4} {'win':>4} {'loss':>4} {'to':>3} {'pnl':>7} {'hold':>5}  obi  voi  decay")
for g in grid[:12]:
    exp, n, w, l, f, p, h, obi_th, voi_entry, dec = g
    print(
        f"{exp:>6.2f} {n:>4} {w:>4} {l:>4} {f:>3} {p:>7.1f} {h:>5.1f}  "
        f"{obi_th:.2f} {voi_entry:.1f} {dec}"
    )
