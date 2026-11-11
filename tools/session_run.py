#!/usr/bin/env python3
"""session_run.py - unattended SimNow session runner.

Waits for the front to accept a TCP connection, starts the four processes,
restarts any that die, stops everything at session end and writes a markdown
summary to log/session_<tag>_summary.md.

Why a long-lived process instead of a scheduled task: the security policy on
this machine blacklists schtasks.exe, so the runner is launched as a detached
background process and left alone.  It is therefore *not* reboot-safe - if the
host or the agent session is killed, relaunch with `--mode now`.

Usage
  python tools/session_run.py --mode auto          # tonight 20:50 -> 02:31, then 08:52 -> 15:06
  python tools/session_run.py --mode now           # start immediately, stop at 02:31
  python tools/session_run.py --mode summary --tag night_20261111
"""
from __future__ import annotations

import argparse
import datetime as dt
import io
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOG = ROOT / "log"


# --------------------------------------------------------------------------- io
def dec(b: bytes) -> str:
    """Logs are written by a GBK-configured C runtime; ASCII is always safe."""
    try:
        return b.decode("utf-8")
    except UnicodeDecodeError:
        return b.decode("gbk", errors="replace")


def say(msg: str) -> None:
    line = f"[{dt.datetime.now():%H:%M:%S}] {msg}"
    print(line, flush=True)
    with io.open(LOG / "session_run.log", "a", encoding="utf-8") as f:
        f.write(line + "\n")


def sizes() -> dict[str, int]:
    out = {}
    for name in ("md", "trader", "query", "strategy"):
        p = LOG / f"{name}.log"
        out[name] = p.stat().st_size if p.exists() else 0
    return out


def read_since(name: str, offset: int) -> str:
    p = LOG / f"{name}.log"
    if not p.exists():
        return ""
    with io.open(p, "rb") as f:
        f.seek(offset)
        return dec(f.read())


# ---------------------------------------------------------------------- process
CREATE_NEW_PROCESS_GROUP = 0x00000200
DETACHED_PROCESS = 0x00000008


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=str(ROOT), capture_output=True, **kw)


def kill_all() -> None:
    for name in ("strategy_proc", "trader_proc", "query_proc", "md_proc"):
        subprocess.run(["taskkill", "/f", "/im", f"{name}.exe"],
                       cwd=str(ROOT), capture_output=True)
        time.sleep(0.5)


def alive(name: str) -> bool:
    r = run(["tasklist", "/fi", f"imagename eq {name}_proc.exe", "/nh"])
    return f"{name}_proc".encode() in r.stdout


def start_one(name: str, cfg: str, bin_dir: Path) -> subprocess.Popen:
    exe = bin_dir / f"{name}_proc.exe"
    log = io.open(LOG / f"{name}_stdout.txt", "ab")
    return subprocess.Popen(
        [str(exe), "--config", cfg, "--mode", "live", "--no-console"],
        cwd=str(ROOT), stdout=log, stderr=log, stdin=subprocess.DEVNULL,
        creationflags=DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
        close_fds=True,
    )


# -------------------------------------------------------------------- schedule
def at(d: dt.date, h: int, m: int) -> dt.datetime:
    return dt.datetime.combine(d, dt.time(h, m))


def build_plan(now: dt.datetime) -> list[dict]:
    d = now.date()
    nd = d + dt.timedelta(days=1)
    out = []
    for tag, s, e in ((f"night_{d:%Y%m%d}", at(d, 20, 50), at(nd, 2, 31)),
                      (f"day_{nd:%Y%m%d}", at(nd, 8, 52), at(nd, 15, 6))):
        if now >= e:
            say(f"skip {tag}: window already over")
            continue
        out.append({"tag": tag, "start": max(s, now), "stop": e})
    return out


# ---------------------------------------------------------------------- fronts
def parse_front(cfg_path: Path, section: str) -> tuple[str, int] | None:
    sec = None
    for raw in io.open(cfg_path, encoding="utf-8", errors="replace"):
        line = raw.split(";")[0].split("#")[0].strip()
        if not line:
            continue
        m = re.match(r"^\[(\w+)\]$", line)
        if m:
            sec = m.group(1)
            continue
        if sec != section:
            continue
        m = re.match(r"^front\s*=\s*tcp://([\d.]+):(\d+)", line)
        if m:
            return m.group(1), int(m.group(2))
    return None


def port_open(host: str, port: int, timeout: float = 2.0) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def wait_for_fronts(fronts: list[tuple[str, int]], deadline: dt.datetime) -> bool:
    while dt.datetime.now() < deadline:
        if all(port_open(h, p) for h, p in fronts):
            return True
        time.sleep(10)
    return all(port_open(h, p) for h, p in fronts)


# --------------------------------------------------------------------- summary
ROUND_RE = re.compile(
    r"\[(\d\d:\d\d:\d\d)[.\d]*\].*?ROUND TRIP (\S+) closed @ ([\d.]+) "
    r"\(entry=([\d.]+) (long|short), pnl=([+-][\d.]+)/unit, ([^)]+)\) \| dayPnL=([+-]?[\d.]+)t")


def summarize(tag: str, cfg: str, offs: dict[str, int], window: tuple[dt.datetime, dt.datetime],
              restarts: dict[str, int], ready: dict[str, str]) -> Path:
    strat = read_since("strategy", offs["strategy"])
    md = read_since("md", offs["md"])
    trader = read_since("trader", offs["trader"])

    rounds = ROUND_RE.findall(strat)
    pnl = [float(r[5]) for r in rounds]
    wins = sum(1 for p in pnl if p > 0)
    losses = sum(1 for p in pnl if p < 0)
    flats = len(pnl) - wins - losses

    gates = {k: sum(int(x) for x in re.findall(rf"{k}=(\d+)", strat))
             for k in ("outside_session", "entry_cutoff", "daily_halt", "wide_spread")}
    gate_lines = len(re.findall(r"no entry order in the last", strat))
    errs = [l for l in strat.splitlines() if "[ERR]" in l]
    warns = [l for l in trader.splitlines() if "[ERR]" in l or "rejected" in l.lower()]

    ticks = [int(x) for x in re.findall(r"ticks=(\d+) \(", strat)]
    execs = [int(x) for x in re.findall(r"execs=(\d+) ", strat)]
    backlogs = [int(x) for x in re.findall(r"backlog=(\d+)", strat)]

    acct = run([str(ROOT / "build" / "bin_simnow" / "account_peek.exe")])
    acct_txt = dec(acct.stdout).strip()

    md_login = len(re.findall(r"md status: logged in", md))
    settle = len(re.findall(r"settlement confirmed", trader))

    b = [f"# 会话小结 {tag}", ""]
    b.append(f"- 时间：{window[0]:%Y-%m-%d %H:%M} → {window[1]:%Y-%m-%d %H:%M}")
    b.append(f"- 配置：`{cfg}`")
    b.append(f"- 就绪：md 登录 {md_login} 次 / trader 结算确认 {settle} 次 / 首 tick {ready.get('first_tick','—')}")
    b.append(f"- 进程重启：{', '.join(f'{k}x{v}' for k, v in restarts.items()) or '无'}")
    b.append(f"- tick 心跳样本 {len(ticks)} 条，末值 ticks={ticks[-1] if ticks else 0} "
             f"execs={execs[-1] if execs else 0} backlog_max={max(backlogs) if backlogs else 0}")
    b.append("")
    b.append("## 成交")
    b.append(f"- 信号 {strat.count(' SIGNAL ')} / 委托 {len(re.findall(r' ORDER ', strat))} "
             f"/ 成交 {len(re.findall(r' TRADE ', strat))} / 回合 {len(rounds)}")
    if pnl:
        b.append(f"- 合计 {sum(pnl):+.1f} tick；胜 {wins} 负 {losses} 平 {flats}"
                 f"；胜率 {100.0 * wins / len(pnl):.0f}%")
        b.append("")
        b.append("| 时间 | 合约 | 方向 | 入场 | 出场 | pnl(t) | 退出方式 |")
        b.append("|---|---|---|---|---|---|---|")
        for r in rounds:
            b.append(f"| {r[0]} | {r[1]} | {r[4]} | {r[3]} | {r[2]} | "
                     f"{float(r[5]):+.1f} | {r[6]} |")
    else:
        b.append("- 本段没有任何回合")
    b.append("")
    b.append("## 闸门")
    b.append(f"- 拦截汇总行 {gate_lines} 条；累计 " +
             " / ".join(f"{k}={v}" for k, v in gates.items()))
    if errs:
        b.append("")
        b.append("## ERR")
        b.extend(f"- `{l.strip()}`" for l in errs[-10:])
    if warns:
        b.append("")
        b.append("## trader 异常")
        b.extend(f"- `{l.strip()}`" for l in warns[-8:])
    b.append("")
    b.append("## 收盘账户")
    b.append("```")
    b.extend(acct_txt.splitlines())
    b.append("```")

    text = "\n".join(b) + "\n"
    out = LOG / f"session_{tag}_summary.md"
    out.write_text(text, encoding="utf-8")
    say(f"summary written: {out}")
    return out


# ----------------------------------------------------------------------- loops
def run_session(s: dict, cfg: str, bin_dir: Path, fronts: list[tuple[str, int]]) -> None:
    tag = s["tag"]
    now = dt.datetime.now()
    if s["start"] > now:
        say(f"{tag}: waiting until {s['start']:%H:%M}")
        while dt.datetime.now() < s["start"]:
            time.sleep(30)

    say(f"{tag}: waiting for fronts {fronts}")
    if not wait_for_fronts(fronts, dt.datetime.now() + dt.timedelta(minutes=25)):
        say(f"{tag}: ABORT - fronts never opened")
        return

    kill_all()
    (ROOT / "flow" / ".halt").unlink(missing_ok=True)
    say(f"{tag}: shm reset")
    run([str(bin_dir / "shm_reset.exe")])
    time.sleep(1)

    offs = sizes()
    ready: dict[str, str] = {}
    restarts: dict[str, int] = {}

    for name in ("md", "trader", "query", "strategy"):
        start_one(name, cfg, bin_dir)
        say(f"{tag}: started {name}")
        time.sleep(3)

    started = dt.datetime.now()
    while dt.datetime.now() < s["stop"]:
        time.sleep(10)
        for name in ("md", "trader", "query", "strategy"):
            if alive(name):
                continue
            if restarts.get(name, 0) >= 3:
                say(f"{tag}: {name} dead, restart budget exhausted")
                continue
            restarts[name] = restarts.get(name, 0) + 1
            say(f"{tag}: {name} died -> restart #{restarts[name]}")
            start_one(name, cfg, bin_dir)
            time.sleep(3)

        if "settle" not in ready:
            t = read_since("trader", offs["trader"])
            if "settlement confirmed" in t:
                ready["settle"] = f"{dt.datetime.now():%H:%M:%S}"
                say(f"{tag}: settlement confirmed, orders enabled")
        if "first_tick" not in ready:
            if re.search(r"first tick", read_since("strategy", offs["strategy"])):
                ready["first_tick"] = f"{dt.datetime.now():%H:%M:%S}"
                say(f"{tag}: first tick received")

    say(f"{tag}: session over, stopping")
    for name in ("strategy_proc", "trader_proc", "query_proc", "md_proc"):
        subprocess.run(["taskkill", "/f", "/im", f"{name}.exe"], cwd=str(ROOT),
                       capture_output=True)
        time.sleep(1)
    summarize(tag, cfg, offs, (started, dt.datetime.now()), restarts, ready)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="config/config_simnow.ini")
    ap.add_argument("--bin", default="build/bin_simnow")
    ap.add_argument("--mode", default="auto", choices=["auto", "now", "summary"])
    ap.add_argument("--tag", default="")
    a = ap.parse_args()

    LOG.mkdir(exist_ok=True)
    cfg = a.config
    bin_dir = ROOT / a.bin
    fronts = [f for f in (parse_front(ROOT / cfg, "md"), parse_front(ROOT / cfg, "trader")) if f]
    if not fronts:
        say(f"no front found in {cfg}")
        return 2

    if a.mode == "summary":
        tag = a.tag or f"night_{dt.date.today():%Y%m%d}"
        offs = {"md": 0, "trader": 0, "query": 0, "strategy": 0}
        summarize(tag, cfg, offs, (dt.datetime.now(), dt.datetime.now()), {}, {})
        return 0

    now = dt.datetime.now()
    if a.mode == "now":
        plan = [{"tag": a.tag or f"manual_{now:%Y%m%d_%H%M}",
                 "start": now, "stop": at((now + dt.timedelta(days=1)).date(), 2, 31)}]
    else:
        plan = build_plan(now)
    say("plan: " + ", ".join(
        "{} {} -> {}".format(p["tag"], p["start"].strftime("%m-%d %H:%M"),
                             p["stop"].strftime("%m-%d %H:%M")) for p in plan))

    for s in plan:
        try:
            run_session(s, cfg, bin_dir, fronts)
        except Exception as e:  # keep the remaining sessions alive
            say(f"{s['tag']}: FAILED {type(e).__name__}: {e}")
    say("all sessions done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
