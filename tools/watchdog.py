#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""watchdog.py - keep the session runner alive across session / console deaths.

Why this exists
---------------
session_run.py used to be started from an agent shell.  Such a shell owns a job
object with KILL_ON_JOB_CLOSE, so when the shell went away the runner *and* its
four DETACHED_PROCESS children were terminated in the same millisecond - no
stack trace, no [ERR] line, no Windows event.  That is how night_20261112 lost
21:03:46 -> 02:30 of trading.

This script is the tripwire.  It is driven by the scheduled task
`ctp_hft_watchdog` (logon + every 5 minutes), so it does not depend on any
terminal staying open.  Each run:

  * reads log/.session_run.heartbeat (written every loop by the runner),
  * if the window is live and the heartbeat is missing / stale / its pid is
    gone, re-creates the runner through WMI (Win32_Process.Create), which puts
    it outside every job object,
  * appends one line to log/watchdog.log either way.

The script is deliberately short-lived: a task *instance* that lingers gets its
children reaped, so it starts the runner under WmiPrvSE and exits.

Usage
    python tools\\watchdog.py            # one check (task does this every 5 min)
    python tools\\watchdog.py --loop     # resident: check every 60 s (self-owning)
    python tools\\watchdog.py --status   # report only, never start anything
    python tools\\watchdog.py --force    # start the runner even outside a window

Single instance: the resident loop binds 127.0.0.1:49518, so a second copy exits
immediately.  It is started by session_run.py (via WMI) and by the logon shortcut
in shell:startup, so no elevated scheduled task is needed on this machine.
"""
from __future__ import annotations

import argparse
import datetime as dt
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOG = ROOT / "log"
HEARTBEAT = LOG / ".session_run.heartbeat"
PIDFILE = LOG / ".session_run.pid"
WLOG = LOG / "watchdog.log"
RUNNER = ROOT / "tools" / "session_run.py"
CONFIG = "config/config_simnow.ini"
BIN = "build/bin_simnow"

STALE_S = 180          # heartbeat older than this means the runner is gone
CREATE_NO_WINDOW = 0x08000000
LOCK_PORT = 49518      # resident-loop single-instance mutex


def log(msg: str) -> None:
    line = f"[{dt.datetime.now():%Y-%m-%d %H:%M:%S}] {msg}"
    print(line, flush=True)
    try:
        with open(WLOG, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def in_window(now: dt.datetime | None = None) -> tuple[bool, str] | tuple[bool, str]:
    """SimNow windows: night 20:50 -> 02:31, day 08:52 -> 15:06, Mon-Fri only."""
    now = now or dt.datetime.now()
    t = now.time()
    wd = now.weekday()            # 0 = Monday
    if wd <= 4 and dt.time(8, 52) <= t <= dt.time(15, 6):
        return True, f"day_{now:%Y%m%d}"
    if 20 <= now.hour or t <= dt.time(2, 31):
        # the night session belongs to the weekday it opened on
        opened = now if now.hour >= 20 else now - dt.timedelta(days=1)
        if opened.weekday() <= 4:
            return True, f"night_{opened:%Y%m%d}"
    return False, ""


def alive(pid: int) -> bool:
    """OpenProcess with SYNCHRONIZE - cheaper and safer than tasklist parsing."""
    import ctypes
    import ctypes.wintypes as wt
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    h = k.OpenProcess(0x00100000, False, pid)      # SYNCHRONIZE
    if not h:
        return False
    state = k.WaitForSingleObject(h, 0)
    k.CloseHandle(h)
    return state == 0x00000102                    # WAIT_TIMEOUT -> still running


def runner_state() -> tuple[bool, str]:
    if not HEARTBEAT.exists():
        return False, "no heartbeat file"
    try:
        stamp, pid_s = HEARTBEAT.read_text(encoding="utf-8").split()
        stamp, pid = float(stamp), int(pid_s)
    except (OSError, ValueError):
        return False, "heartbeat unreadable"
    age = dt.datetime.now().timestamp() - stamp
    if not alive(pid):
        return False, f"pid {pid} gone, heartbeat {age:.0f}s old"
    if age > STALE_S:
        return False, f"pid {pid} alive but silent for {age:.0f}s"
    return True, f"pid {pid} heartbeat {age:.0f}s"


def spawn_runner(tag: str) -> int:
    argv = [sys.executable, str(RUNNER), "--mode", "auto",
            "--config", CONFIG, "--bin", BIN, "--orphan"]
    if tag:
        argv += ["--tag", tag]
    cmdline = " ".join(f'"{a}"' for a in argv)
    script = (
        "$ErrorActionPreference='Stop';"
        "$r=Invoke-CimMethod -ClassName Win32_Process -MethodName Create "
        f"-Arguments @{{CommandLine='{cmdline}';CurrentDirectory='{ROOT}'}};"
        "if($r.ReturnValue -ne 0){Write-Output ('ERR ' + $r.ReturnValue);exit 1};"
        "$r.ProcessId"
    )
    r = subprocess.run(
        ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
         "-WindowStyle", "Hidden", "-Command", script],
        capture_output=True, text=True, timeout=120, creationflags=CREATE_NO_WINDOW,
    )
    out = (r.stdout or "").strip()
    if r.returncode != 0 or not out.isdigit():
        raise RuntimeError((out or r.stderr or "WMI create failed").strip()[:300])
    return int(out)


def check_and_heal(force: bool = False, dry: bool = False) -> int:
    ok, why = runner_state()
    live, tag = in_window()
    where = tag or "off-window"

    if ok:
        log(f"{where}: runner healthy ({why})")
        return 0
    if not live and not force:
        log(f"{where}: runner down ({why}) - outside the session window, not starting")
        return 0
    if dry:
        log(f"{where}: runner DOWN ({why}) - would start it here (--status: no action)")
        return 1

    try:
        pid = spawn_runner(tag if live else f"manual_{dt.datetime.now():%Y%m%d_%H%M}")
    except Exception as e:
        log(f"{where}: runner down ({why}) - START FAILED: {e}")
        return 2
    log(f"{where}: runner down ({why}) - started pid={pid} under WmiPrvSE")
    return 0


def loop() -> int:
    """Resident supervisor.  Binds LOCK_PORT so only one copy ever runs."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        srv.bind(("127.0.0.1", LOCK_PORT))
        srv.listen(4)
    except OSError:
        print("another watchdog holds the lock - exiting")
        return 0
    log(f"watchdog loop started, pid={os.getpid()} lock=127.0.0.1:{LOCK_PORT}")
    while True:
        try:
            check_and_heal()
        except Exception as e:      # the supervisor must never die of its own bug
            log(f"check failed: {type(e).__name__}: {e}")
        time.sleep(60)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--status", action="store_true", help="report only")
    ap.add_argument("--force", action="store_true", help="start even outside a window")
    ap.add_argument("--loop", action="store_true", help="resident supervisor, 60 s tick")
    a = ap.parse_args()
    if a.loop:
        return loop()
    return check_and_heal(force=a.force, dry=a.status)


if __name__ == "__main__":
    sys.exit(main())
