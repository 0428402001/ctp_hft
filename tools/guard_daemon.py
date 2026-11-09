#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""guard_daemon.py - the always-on supervisor for ctp_hft.

Why a daemon and not "start a task every time we need a restart"
---------------------------------------------------------------
Windows kills the process tree of a scheduled-task *instance* when that
instance goes away.  Triggering ctp_hft_launch on every restart therefore made
earlier instances' children (md / trader / query) disappear minutes later, and
every restart also re-ran the CTP login - which SimNow answers with a throttle
kick (reason=4097) after a few times.

So the task now starts exactly ONE long-lived process: this daemon.  It owns
the four children, so their lifetime is tied to the daemon, not to a task
instance that comes and goes.  Restart decisions - including the 09:00/16:00
day <-> 724 front switch - happen here, on a timer.

Cooperation with tools/guard.py
-------------------------------
guard.py is the interactive / automation-facing tool.  It never starts trading
processes itself; it writes flow\\.restart_request and this daemon carries it
out.  Requests are honoured even inside the reconnect cooldown when they carry
"force": true, but the cooldown always applies to automatic repairs.

Usage
-----
    python tools\\guard_daemon.py            # run in the foreground
    (normally started by the scheduled task "ctp_hft_daemon")
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import guard  # noqa: E402  (ENVS, PROCS, BIN, ROOT, desired_env, ...)

DAEMON_LOG = guard.LOG_DIR / "guard_daemon.log"
PIDFILE = guard.LOG_DIR / ".guard_daemon.pid"
REQUEST = guard.FLOW_DIR / ".restart_request"
LOCK_PORT = 49517

TICK_S = 20            # health check interval
COOLDOWN_S = 180       # between two *automatic* recycles (front throttle)
WINDOW_S = 900
MAX_AUTO = 3           # automatic recycles per window

_state = {"last_auto": 0.0, "history": []}


def log(msg: str) -> None:
    line = f"[{datetime.now():%Y-%m-%d %H:%M:%S}] {msg}"
    guard.LOG_DIR.mkdir(parents=True, exist_ok=True)
    with DAEMON_LOG.open("a", encoding="utf-8") as f:
        f.write(line + "\n")


def single_instance() -> bool:
    """A bound TCP port is a cheap, crash-safe mutex on Windows."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", LOCK_PORT))
        s.listen(8)
    except OSError:
        return False
    globals()["_lock_sock"] = s

    # Drain probes: guard.py connects here to ask "are you alive?".  Without a
    # running accept the backlog fills after a few checks and every later probe
    # fails, which made --status report the daemon as DOWN while it was fine.
    def _drain():
        while True:
            try:
                c, _ = s.accept()
                c.close()
            except OSError:
                return

    threading.Thread(target=_drain, daemon=True).start()
    return True


def _child_out(name: str):
    """Give every child its own stdout/stderr file so a crash leaves a trace."""
    return open(guard.LOG_DIR / f"daemon_{name}.out", "ab", buffering=0)


def cold_start(env: str, reason: str) -> bool:
    cfg = guard.ENVS[env]["config"]
    if not cfg.exists():
        log(f"ABORT {reason}: config missing {cfg}")
        return False

    log(f"cold start env={env} ({reason})")
    guard.kill_all()
    time.sleep(1)

    exe = guard.BIN / "shm_reset.exe"
    try:
        subprocess.run([str(exe)], cwd=str(guard.ROOT), capture_output=True,
                       timeout=30, creationflags=0x08000000)
    except Exception as e:  # noqa: BLE001
        log(f"shm_reset failed: {e}")

    order = [("md_proc.exe", 4), ("trader_proc.exe", 4),
             ("query_proc.exe", 4), ("strategy_proc.exe", 0)]
    for name, delay in order:
        try:
            p = subprocess.Popen(
                [str(guard.BIN / name), "--config", str(cfg),
                 "--mode", "live", "--no-console"],
                cwd=str(guard.ROOT),
                stdin=subprocess.DEVNULL,
                stdout=_child_out(name),
                stderr=subprocess.STDOUT,
                creationflags=0x00000008 | 0x00000200,   # DETACHED | NEW_GROUP
                close_fds=True,
            )
            log(f"  started {name} pid={p.pid}")
        except Exception as e:  # noqa: BLE001
            log(f"  FAILED to start {name}: {e}")
            return False
        if delay:
            time.sleep(delay)

    guard.write_marker(env)
    return True


def restart_strategy(env: str, reason: str) -> bool:
    """Swap strategy_proc.exe alone; md/trader/query keep their CTP sessions.

    A full cold start re-logs in on the front, which is what SimNow throttles
    (reason=4097).  When only the strategy died there is no reason to pay that.
    """
    cfg = guard.ENVS[env]["config"]
    if not cfg.exists():
        log(f"ABORT {reason}: config missing {cfg}")
        return False

    log(f"strategy-only restart ({reason})")
    subprocess.run(["taskkill", "/F", "/IM", "strategy_proc.exe"],
                   capture_output=True, timeout=20, creationflags=0x08000000)
    time.sleep(1)
    try:
        p = subprocess.Popen(
            [str(guard.BIN / "strategy_proc.exe"), "--config", str(cfg),
             "--mode", "live", "--no-console"],
            cwd=str(guard.ROOT),
            stdin=subprocess.DEVNULL,
            stdout=_child_out("strategy_proc.exe"),
            stderr=subprocess.STDOUT,
            creationflags=0x00000008 | 0x00000200,   # DETACHED | NEW_GROUP
            close_fds=True,
        )
    except Exception as e:  # noqa: BLE001
        log(f"  FAILED to start strategy_proc.exe: {e}")
        return False
    log(f"  started strategy_proc.exe pid={p.pid}")
    return True


def auto_allowed(now_ts: float) -> str | None:
    if now_ts - _state["last_auto"] < COOLDOWN_S:
        return f"cooldown {int(COOLDOWN_S - (now_ts - _state['last_auto']))}s"
    hist = [t for t in _state["history"] if now_ts - t < WINDOW_S]
    _state["history"] = hist
    if len(hist) >= MAX_AUTO:
        return f"{len(hist)} auto restarts in {WINDOW_S // 60}min"
    return None


def mark_auto(now_ts: float) -> None:
    _state["last_auto"] = now_ts
    _state["history"].append(now_ts)


# ---- front throttle guard ---------------------------------------------------
# SimNow rate-limits by IP: once we have reconnected a few times in a row, every
# login comes back "startup failed ... login timeout" with reason=4097 right
# after connect, and every further attempt extends the ban.  Seen 2026-11-05
# (17:19/17:24/17:36) and again 2026-11-06 09:00-09:12, when BOTH front groups
# (30001 and 40001) refused the account for ~10 min while the operator kept
# trying.  The only cure is to stop knocking: kill the tree, sit still, come back
# with a single attempt.
FRONT_COOLDOWN_S = 300         # length of the silence
FRONT_FAIL_WINDOW_S = 180      # count login failures inside this window
FRONT_FAIL_LIMIT = 6           # this many -> we are being throttled
THROTTLE_MARK = guard.LOG_DIR / ".front_throttle"


def _log_stamp_age(line: str, now_ts: float) -> float | None:
    """Age in seconds of a '[HH:MM:SS.mmm]' log line, or None."""
    try:
        hh, mm, ss = line[1:9].split(":")
        now = time.localtime(now_ts)
        midnight = now_ts - (now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec)
        age = now_ts - (midnight + int(hh) * 3600 + int(mm) * 60 + float(ss))
        return age if age >= -5 else age + 86400     # just past midnight
    except Exception:
        return None


def recent_login_failures(now_ts: float) -> int:
    path = guard.LOG_DIR / "trader.log"
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - 400_000))
            chunk = f.read().decode("utf-8", "replace")
    except Exception:
        return 0
    n = 0
    for line in chunk.splitlines():
        if "startup failed" not in line and "login timeout" not in line:
            continue
        age = _log_stamp_age(line, now_ts)
        if age is not None and -5 <= age <= FRONT_FAIL_WINDOW_S:
            n += 1
    return n


def throttle_gate(now_ts: float) -> bool:
    """True = stay away from the front this tick."""
    try:
        until = float(THROTTLE_MARK.read_text(encoding="utf-8").strip() or 0)
    except Exception:
        until = 0.0

    if until and now_ts < until:
        return True                       # still cooling down

    if until:                             # cooldown finished
        THROTTLE_MARK.unlink(missing_ok=True)
        _state["history"] = []            # fresh budget: one clean attempt
        _state["last_auto"] = 0.0
        log("front cooldown finished - restart budget reset, retrying once")

    fails = recent_login_failures(now_ts)
    if fails >= FRONT_FAIL_LIMIT:
        THROTTLE_MARK.write_text(f"{now_ts + FRONT_COOLDOWN_S:.0f}\n", encoding="utf-8")
        log(f"FRONT THROTTLED: {fails} login failures in {FRONT_FAIL_WINDOW_S}s -> "
            f"stopping the tree and staying silent {FRONT_COOLDOWN_S}s "
            "(more attempts only extend SimNow's ban)")
        guard.kill_all()
        _state["history"] = []
        _state["last_auto"] = 0.0
        return True
    return False


def handle_request() -> bool:
    """Carry out what guard.py asked for.  Returns True if it did anything."""
    if not REQUEST.exists():
        return False
    try:
        req = json.loads(REQUEST.read_text(encoding="utf-8-sig"))
    except Exception:
        REQUEST.unlink(missing_ok=True)
        return False
    REQUEST.unlink(missing_ok=True)

    env = req.get("env") or guard.desired_env()
    mode = req.get("mode", "full")
    force = bool(req.get("force"))

    log(f"request: env={env} mode={mode} force={force}")

    if mode == "strategy":
        # swap one process, keep the three CTP sessions
        if not guard.in_service_window(env):
            log("  refused: front not in service window")
            return False
        subprocess.run(["taskkill", "/F", "/IM", "strategy_proc.exe"],
                       capture_output=True, creationflags=0x08000000)
        time.sleep(1)
        cfg = guard.ENVS[env]["config"]
        p = subprocess.Popen(
            [str(guard.BIN / "strategy_proc.exe"), "--config", str(cfg),
             "--mode", "live", "--no-console"],
            cwd=str(guard.ROOT), stdin=subprocess.DEVNULL,
            stdout=_child_out("strategy_proc.exe"), stderr=subprocess.STDOUT,
            creationflags=0x00000008 | 0x00000200, close_fds=True)
        log(f"  strategy restarted pid={p.pid}")
        return True

    # A request in flow/.restart_request is an explicit human instruction:
    # guard.py already ran its own throttle before writing it.  Re-checking
    # here refused a healthy resume on 2026-11-06 13:16 (guard.py allowed,
    # the daemon said "3 auto restarts in 15min", consumed the request and
    # started nothing).  Crash-loop / front-ban protection stays active:
    # throttle_gate() runs before handle_request() in every tick, and the
    # auto-repair path below still uses auto_allowed().
    ok = cold_start(env, "manual request" if force else "request")
    if ok:
        mark_auto(time.time())
    return ok


def tick() -> None:
    now_ts = time.time()

    # ---- hard stop switch ---------------------------------------------------
    # The keeper writes flow\.halt when a risk limit trips.  While it exists the
    # daemon refuses to repair anything: an automatic restart would quietly undo
    # the human decision to stop trading.  Remove the file to resume.
    if (guard.FLOW_DIR / ".halt").exists():
        if not _state.get("halt_logged"):
            log("HALT file present - automatic repair disabled "
                "(delete flow\\.halt to resume)")
            _state["halt_logged"] = True
        return
    _state["halt_logged"] = False

    if throttle_gate(now_ts):
        return

    if handle_request():
        return

    env = guard.desired_env()
    alive = guard.running_procs()
    if alive is None:
        # Could not read the process table.  This is NOT "everything died":
        # treating it that way is what made the tree vanish at 17:19/17:24/17:36
        # today (kill_all + cold start, no log line, no crash).
        log("probe failed (tasklist + powershell) - skipping this tick, "
            "not touching the tree")
        return
    marker = guard.read_marker()
    missing = [p for p in guard.PROCS if p not in alive]
    env_mismatch = (marker != env)

    if not missing and not env_mismatch:
        return

    if not guard.in_service_window(env):
        return                     # outside the front's hours: nothing to fix

    if missing:
        # Second opinion before destroying anything: a single bad probe must
        # never be enough to kill four healthy processes.
        time.sleep(3)
        again = guard.running_procs()
        if again is None:
            log(f"confirm probe failed (first missing={missing or 'none'}) - "
                "skipping this tick")
            return
        missing2 = [p for p in guard.PROCS if p not in again]
        if not missing2:
            log(f"probe disagreement (first missing={missing or 'none'}, "
                "second: all alive) - keeping the tree")
            return
        missing = missing2

        # Re-check the halt switch before touching anything.  The tick starts
        # with one halt check, but the second-opinion probe above sleeps 3 s and
        # `status()` itself shells out to tasklist - on 2026-11-06 11:31:04 that
        # window was wide enough for the daemon to restart the OLD strategy in
        # the middle of a rebuild (the halt file had been raised 7 s earlier,
        # after the tick's first check).  A build script raises .halt for the
        # whole compile window; honour it here as well.
        if (guard.FLOW_DIR / ".halt").exists():
            log("halt appeared during this tick - aborting repair")
            return

        # Only strategy died: swap it on its own.  Keeping md/trader/query alive
        # avoids a fresh front login and spares SimNow's reconnect quota.
        if missing == ["strategy_proc.exe"]:
            why = auto_allowed(now_ts)
            if why:
                log(f"strategy missing but {why}")
                return
            if restart_strategy(env, "auto repair: strategy only"):
                mark_auto(now_ts)
            return

    # Either something died, or the clock crossed a front boundary while the
    # tree is perfectly healthy.  The second case used to fall into the probe
    # cross-check above and get "kept" forever, so day <-> 724 never actually
    # switched - that is why the system sat on the 724 front all through the
    # 2026-11-06 morning.  An environment change is a planned switch, not a
    # fault: go straight to the cold start.
    why = auto_allowed(now_ts)
    if why:
        log(f"unhealthy (missing={missing or 'none'} env={marker}/{env}) but {why}")
        return

    found = "" if not env_mismatch else " (front switched)"
    if cold_start(env, f"auto repair: missing={missing or 'none'} env={marker}->{env}{found}"):
        mark_auto(now_ts)


def main() -> int:
    if not single_instance():
        print("guard_daemon: another instance is already running")
        return 0

    PIDFILE.write_text(str(os.getpid()), encoding="utf-8")
    log(f"daemon up (pid={os.getpid()}) tick={TICK_S}s "
        f"day={guard.ENVS['day']['config'].name} 724={guard.ENVS['724']['config'].name}")

    # --restart-once: on first launch, take ownership of the children.  Any
    # process started by an earlier task instance can be reaped by Windows when
    # that instance is cleaned up, so a daemon-owned tree is the only stable one.
    if "--restart-once" in sys.argv:
        env = guard.desired_env()
        if guard.in_service_window(env):
            cold_start(env, "initial takeover")
        else:
            log(f"initial takeover skipped: {env} front not serving now")

    guard.LOG_DIR.mkdir(parents=True, exist_ok=True)
    last_beat = 0.0
    while True:
        try:
            tick()
        except Exception as e:  # noqa: BLE001
            log(f"tick failed: {e}")
        if time.time() - last_beat > 120:
            alive = guard.running_procs()
            n = "?" if alive is None else len(alive)
            log(f"beat procs={n}/4 env={guard.read_marker()} "
                f"desired={guard.desired_env()}")
            last_beat = time.time()
        time.sleep(TICK_S)


if __name__ == "__main__":
    sys.exit(main())
