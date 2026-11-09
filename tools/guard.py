#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
guard.py - 7x24 supervisor for ctp_hft (SimNow).

Why this exists
---------------
SimNow exposes two mutually exclusive front groups for the same account:

    day  (set 1)  trade 30001 / md 30011   serves the day sessions 09:00-16:00
    724  (set 2)  trade 40001 / md 40011   serves 16:00 -> next 09:00
                                          (non-trading days: 16:00 -> 12:00)

Running "7x24" therefore means *switching* between the two config files at the
09:00 / 16:00 boundaries, not picking one and leaving it.  Both the four
processes and the shared-memory bus must be recycled on every switch: the bus is
shared by name, so two flavours can never be alive at the same time.

The supervisor
--------------
  * works out which environment *should* be live right now,
  * compares it with the marker file flow/.envmode (what is actually live),
  * and only restarts when the environment changed or a process died inside a
    window where the front is known to serve.

Restart policy is deliberately conservative: SimNow throttles the front
(reason=4097, a hard kick) when a client reconnects too often, and a throttled
account stays unusable for several minutes.  So a restart is refused unless at
least COOLDOWN_S has passed since the previous one and fewer than MAX_RESTARTS
restarts happened in the last WINDOW_S.

Usage
-----
    python tools/guard.py                # check, restart only if needed
    python tools/guard.py --status       # report only, never act
    python tools/guard.py --env day      # force the environment to use
    python tools/guard.py --restart      # force a recycle (respects cooldown)
    python tools/guard.py --force        # force a recycle, ignore cooldown
    python tools/guard.py --json         # machine-readable summary
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "bin_simnow"
LOG_DIR = ROOT / "log"
FLOW_DIR = ROOT / "flow"

MARKER = FLOW_DIR / ".envmode"
STATE = LOG_DIR / ".guard_state.json"

PROCS = ["md_proc.exe", "trader_proc.exe", "query_proc.exe", "strategy_proc.exe"]

ENVS = {
    "day": {
        "config": ROOT / "config" / "config_simnow.ini",
        "trade": ("182.254.243.31", 30001),
        "md": ("182.254.243.31", 30011),
    },
    "724": {
        "config": ROOT / "config" / "config_simnow724.ini",
        "trade": ("182.254.243.31", 40001),
        "md": ("182.254.243.31", 40011),
    },
}

# ---- restart throttle -------------------------------------------------------
COOLDOWN_S = 180          # min gap between two recycles (front kicks us at ~4097)
WINDOW_S = 900            # throttle window
MAX_RESTARTS = 3          # max recycles inside WINDOW_S

# ---- session windows (local time) -------------------------------------------
DAY_OPEN, DAY_CLOSE = 9 * 60, 16 * 60          # 09:00 - 16:00


def now_str() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


# ---- manual environment override --------------------------------------------
# One line: "<env> <expiry epoch>".  Written by --force-env, honoured by both
# guard.py and the daemon until it expires, so an operator can pin the system to
# the front that actually answers when the scheduled one is down.
ENVFORCE = LOG_DIR / ".envforce"
ENVFORCE_DEFAULT_MIN = 30


def forced_env() -> str | None:
    try:
        parts = ENVFORCE.read_text(encoding="utf-8-sig").split()
    except Exception:
        return None
    if not parts or parts[0] not in ENVS:
        return None
    if len(parts) > 1:
        try:
            if time.time() > float(parts[1]):
                return None          # expired - let the clock decide again
        except ValueError:
            pass
    return parts[0]


def set_forced_env(env: str | None, minutes: int = ENVFORCE_DEFAULT_MIN) -> None:
    FLOW_DIR.mkdir(parents=True, exist_ok=True)
    if env is None:
        try:
            ENVFORCE.unlink()
        except FileNotFoundError:
            pass
        return
    ENVFORCE.write_text(f"{env} {time.time() + minutes * 60:.0f}\n", encoding="utf-8")


def desired_env(now: datetime | None = None) -> str:
    """Which front group serves right now (manual override wins)."""
    forced = forced_env()
    if forced:
        return forced
    now = now or datetime.now()
    m = now.hour * 60 + now.minute
    return "day" if DAY_OPEN <= m < DAY_CLOSE else "724"


def in_service_window(env: str, now: datetime | None = None) -> bool:
    """Conservative: is `env` expected to accept a login right now?"""
    now = now or datetime.now()
    m = now.hour * 60 + now.minute
    if env == "day":
        return DAY_OPEN <= m < DAY_CLOSE
    # 724: 16:00 -> 09:00 on trading days, 16:00 -> 12:00 otherwise.
    # We cannot know the calendar from here, so just accept the broad window.
    return m >= DAY_CLOSE or m < 12 * 60


# ---------------------------------------------------------------- process state
def _procs_via_tasklist() -> tuple[set[str] | None, str]:
    """(alive, error).  `alive is None` means the probe itself failed.

    That distinction matters more than anything else in this file: a failed
    probe used to look exactly like "all four processes are gone", so the
    supervisor killed a healthy tree and cold-started it.  That is what made
    the processes vanish at 17:19, 17:24 and 17:36 today - no log line, no
    crash, just a kill from our own supervisor.
    """
    try:
        r = subprocess.run(
            ["tasklist", "/FO", "CSV", "/NH"],
            capture_output=True, timeout=20,
            creationflags=0x08000000,  # CREATE_NO_WINDOW
        )
    except Exception as e:  # noqa: BLE001
        return None, f"tasklist raised: {e}"
    if r.returncode != 0:
        return None, f"tasklist rc={r.returncode}"
    out = r.stdout.decode("utf-8", "replace")
    if not out.strip():
        return None, "tasklist produced no output"
    lines = [l for l in out.splitlines() if l.strip()]
    # A real process table has 60-150 rows on this box.  Anything shorter means
    # the call was filtered or truncated, so the result cannot be trusted.
    if len(lines) < 10:
        return None, f"tasklist returned only {len(lines)} row(s)"
    alive = set()
    for line in lines:
        name = line.split(",")[0].strip('"').lower()
        if name in PROCS:
            alive.add(name)
    return alive, ""


def _procs_via_powershell() -> set[str] | None:
    """Second opinion through a different API, used to confirm a bad result."""
    names = ",".join(p[:-4] if p.endswith(".exe") else p for p in PROCS)
    script = ("Get-Process -Name " + names +
              " -ErrorAction SilentlyContinue | ForEach-Object { $_.ProcessName }")
    try:
        r = subprocess.run(
            ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
             "-WindowStyle", "Hidden", "-Command", script],
            capture_output=True, timeout=30, creationflags=0x08000000,
        )
    except Exception:  # noqa: BLE001
        return None
    if r.returncode != 0:
        return None
    got = {ln.strip().lower() + ".exe"
           for ln in r.stdout.decode("utf-8", "replace").splitlines() if ln.strip()}
    return {p for p in PROCS if p in got}


def running_procs() -> set[str] | None:
    """Live processes, or None when we genuinely cannot tell.

    None must be treated as "unknown - do nothing this tick".  Waiting 20 s
    costs nothing; killing a healthy tree costs a re-login, a position
    reconcile and another strike against SimNow's reconnect quota.

    A *successful* tasklist is trusted even when it finds none of our processes:
    the row-count guard above already rejects chopped or filtered output, and
    insisting on a PowerShell confirmation here would deadlock the supervisor
    whenever powershell.exe is unavailable (it then never repaired anything).
    """
    alive, _err = _procs_via_tasklist()
    if alive is not None:
        return alive
    return _procs_via_powershell()


def read_marker() -> str | None:
    try:
        return MARKER.read_text(encoding="utf-8-sig").strip()
    except FileNotFoundError:
        return None


def write_marker(env: str) -> None:
    FLOW_DIR.mkdir(parents=True, exist_ok=True)
    MARKER.write_text(env + "\n", encoding="utf-8")


# ------------------------------------------------------------- restart throttle
def load_state() -> dict:
    try:
        return json.loads(STATE.read_text(encoding="utf-8"))
    except Exception:
        return {"last_restart": 0.0, "history": []}


def save_state(st: dict) -> None:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    STATE.write_text(json.dumps(st, indent=2), encoding="utf-8")


def throttle_check(st: dict, now_ts: float) -> str | None:
    """Return a refusal reason, or None when a restart is allowed."""
    last = float(st.get("last_restart") or 0)
    if last and now_ts - last < COOLDOWN_S:
        return f"cooldown {int(COOLDOWN_S - (now_ts - last))}s left"
    hist = [t for t in st.get("history", []) if now_ts - t < WINDOW_S]
    if len(hist) >= MAX_RESTARTS:
        return f"{len(hist)} restarts in {WINDOW_S // 60}min - front throttling risk"
    return None


# ------------------------------------------------------------------- lifecycle
def kill_all() -> None:
    for p in PROCS + ["monitor.exe"]:
        subprocess.run(
            ["taskkill", "/F", "/IM", p],
            capture_output=True, timeout=20, creationflags=0x08000000,
        )
    # give the OS a moment to release the shared-memory handles
    time.sleep(2)


DAEMON_TASK = "ctp_hft_daemon"
DAEMON_PORT = 49517
REQUEST = FLOW_DIR / ".restart_request"


def daemon_running() -> bool:
    """The daemon binds 127.0.0.1:DAEMON_PORT as its single-instance mutex, so a
    successful connect means it is alive and able to take a request."""
    import socket
    try:
        c = socket.create_connection(("127.0.0.1", DAEMON_PORT), timeout=1.0)
        c.close()
        return True
    except OSError:
        return False


def start_daemon() -> dict:
    script = (
        "$ErrorActionPreference='Stop'; "
        f"Start-ScheduledTask -TaskName '{DAEMON_TASK}'; "
        "Start-Sleep -Seconds 2; "
        f"(Get-ScheduledTask -TaskName '{DAEMON_TASK}').State"
    )
    r = subprocess.run(
        ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
         "-WindowStyle", "Hidden", "-Command", script],
        capture_output=True, text=True, timeout=90,
        creationflags=0x08000000,
    )
    return {"rc": r.returncode, "state": (r.stdout or "").strip(),
            "stderr": (r.stderr or "").strip()[:400]}


def recycle(env: str, force: bool = False, strategy_only: bool = False) -> dict:
    """Hand the restart to the always-on daemon.

    We do NOT start trading processes from here: anything spawned inside an
    agent / shell session dies with that session's job object, and anything
    spawned by a *task instance* can be reaped when the instance is cleaned up.
    The daemon owns the children, so its lifetime is the only one that matters.
    """
    cfg = ENVS[env]["config"]
    if not cfg.exists():
        return {"ok": False, "error": f"config not found: {cfg}"}

    mode = "strategy" if strategy_only else "full"

    if not daemon_running():
        res = start_daemon()
        if res["rc"] != 0:
            return {"ok": False, "error": res.get("stderr") or "cannot start daemon"}
        for _ in range(20):
            if daemon_running():
                break
            time.sleep(0.5)

    FLOW_DIR.mkdir(parents=True, exist_ok=True)
    REQUEST.write_text(json.dumps(
        {"env": env, "mode": mode, "force": force, "ts": time.time()},
        ensure_ascii=False), encoding="utf-8")

    # the daemon picks the request up on its next tick (<= 20 s) and removes it
    deadline = time.time() + 30
    while time.time() < deadline and REQUEST.exists():
        time.sleep(0.5)

    time.sleep(14 if not strategy_only else 3)

    st = load_state()
    now_ts = time.time()
    st["last_restart"] = now_ts
    st["history"] = [t for t in st.get("history", []) if now_ts - t < WINDOW_S] + [now_ts]
    st["env"] = env
    save_state(st)

    alive = running_procs()
    if alive is None:
        return {"ok": True, "env": env, "mode": mode,
                "started": [], "missing": [], "probe": "failed"}
    return {"ok": True, "env": env, "mode": mode,
            "started": [p for p in PROCS if p in alive],
            "missing": [p for p in PROCS if p not in alive]}


# -------------------------------------------------------------------- reporting
def log_health(tail_lines: int = 40) -> dict:
    """Very light check that the strategy is still consuming ticks."""
    p = LOG_DIR / "strategy.log"
    if not p.exists():
        return {"strategy_log": "missing"}
    try:
        with p.open("rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - 65536))
            block = f.read().decode("utf-8", "replace")
        lines = [l for l in block.splitlines() if l.strip()][-tail_lines:]
        last_tick = next((l for l in reversed(lines) if "tickdat" in l), None)
        last_err = next((l for l in reversed(lines) if "][ERR]" in l), None)
        age = None
        if last_tick:
            try:
                ts = last_tick[1:13]  # [HH:MM:SS.mmm]
                now = datetime.now()
                stamp = datetime.strptime(ts, "%H:%M:%S.%f").replace(
                    year=now.year, month=now.month, day=now.day)
                if stamp > now:      # the log line is from just before midnight
                    stamp -= timedelta(days=1)
                age = round((now - stamp).total_seconds(), 1)
            except Exception:
                age = None
        return {"last_tick_age_s": age, "last_error": last_err}
    except Exception as e:  # noqa: BLE001
        return {"strategy_log_error": str(e)}


def status(env_now: str) -> dict:
    alive = running_procs()
    probe_failed = alive is None
    if probe_failed:
        alive = set()
    marker = read_marker()
    missing = [p for p in PROCS if p not in alive]
    return {
        "time": now_str(),
        "desired_env": env_now,
        "running_env": marker,
        "daemon": daemon_running(),
        "alive": sorted(alive),
        "missing": missing,
        "probe_failed": probe_failed,
        "healthy": (not probe_failed) and (not missing) and marker == env_now,
        **log_health(),
    }


# ------------------------------------------------------------------------- main
def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--status", action="store_true", help="report only")
    ap.add_argument("--restart", action="store_true", help="recycle")
    ap.add_argument("--force", action="store_true", help="ignore the throttle")
    ap.add_argument("--strategy-only", action="store_true",
                    help="recycle strategy_proc.exe only (keeps the CTP sessions; "
                         "use this after rebuilding the strategy)")
    ap.add_argument("--env", choices=sorted(ENVS), help="force environment")
    ap.add_argument("--force-env", choices=sorted(ENVS), metavar="ENV",
                    help="pin the environment in flow/.envforce for --minutes")
    ap.add_argument("--minutes", type=int, default=ENVFORCE_DEFAULT_MIN,
                    help=f"how long --force-env lasts (default {ENVFORCE_DEFAULT_MIN})")
    ap.add_argument("--clear-force", action="store_true",
                    help="drop flow/.envforce and fall back to the clock")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    if args.clear_force:
        set_forced_env(None)
        print(f"env override cleared; clock now picks '{desired_env()}'")
        return 0
    if args.force_env:
        set_forced_env(args.force_env, args.minutes)
        print(f"env pinned to '{args.force_env}' for {args.minutes} min "
              f"(flow/.envforce)")

    env_now = args.env or desired_env()
    before = status(env_now)

    if args.status:
        print(json.dumps(before, ensure_ascii=False, indent=2) if args.json
              else _fmt(before))
        return 0

    # Anything beyond a --status look means a human decided the system should
    # run again, so drop the stop switch stop_all.bat raised.  A risk-trip
    # halt is equally overridden - explicitly, on the console - and the
    # (currently paused) keeper would re-raise it on its next check anyway.
    halt_path = FLOW_DIR / ".halt"
    if halt_path.exists():
        try:
            halt_path.unlink()
            print("cleared flow/.halt (manual stop switch) - supervision re-armed")
        except OSError:
            pass

    need = (args.restart or args.strategy_only or before["missing"]
            or before["running_env"] != env_now)
    acted = {"restarted": False}
    refusal = None

    if before.get("probe_failed") and not (args.force or args.restart
                                           or args.strategy_only):
        # We could not read the process table.  Never treat that as "everything
        # is dead": doing so is exactly what killed a healthy tree earlier.
        acted["refused"] = "process probe failed - not touching anything"
        need = False

    if need:
        st = load_state()
        if args.force or args.strategy_only:
            # a strategy-only swap does not touch the front, so the reconnect
            # throttle does not apply to it
            pass
        elif args.restart and not args.env:
            # explicit restart still honours the cooldown unless --force
            refusal = throttle_check(st, time.time())
        else:
            refusal = throttle_check(st, time.time())

        if refusal:
            acted["refused"] = refusal
        elif not in_service_window(env_now):
            acted["refused"] = f"{env_now} front not serving at this hour"
        else:
            res = recycle(env_now, strategy_only=args.strategy_only)
            acted = {"restarted": True, **res}

    after = status(env_now)
    out = {"before": before, "after": after, "action": acted}

    if args.json:
        print(json.dumps(out, ensure_ascii=False, indent=2))
    else:
        print(_fmt(after))
        if acted.get("restarted"):
            names = acted.get("started") or []
            scope = "strategy only - CTP sessions kept" if args.strategy_only else "full tree"
            print(f"action   : recycled on '{env_now}' ({scope})")
            # only the names that were actually (re)started, not the whole set
            print("procs    : " + (", ".join(names) if names else "(none reported)")
                  + (f"  (still missing: {','.join(acted['missing'])})"
                     if acted.get("missing") else ""))
        elif acted.get("refused"):
            print(f"action   : restart refused ({acted['refused']})")
        else:
            print("action   : none - all good")

    return 0 if after["healthy"] else 1


def _fmt(s: dict) -> str:
    if s.get("probe_failed"):
        alive = "UNKNOWN (process probe failed)"
    else:
        alive = ",".join(s["alive"]) or "none"
    return (
        f"time     : {s['time']}\n"
        f"env      : desired={s['desired_env']} running={s['running_env']}"
        f"  daemon={'up' if s.get('daemon') else 'DOWN'}\n"
        f"procs    : {alive}"
        + (f"  MISSING={','.join(s['missing'])}"
           if s["missing"] and not s.get("probe_failed") else "") + "\n"
        f"last tick: {s.get('last_tick_age_s')}s ago"
        + (f"\nlast err : {s['last_error']}" if s.get("last_error") else "")
    )


if __name__ == "__main__":
    sys.exit(main())
