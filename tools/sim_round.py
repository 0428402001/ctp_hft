#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sim_round.py - one automated simulation round for ctp_hft.

Runs the four sim processes in the background (no console windows), lets the
strategy trade for --seconds, then reads the query process log (the simulated
book: balance / realized / fee) and the strategy log (round trips) and prints
a JSON summary. Kills all processes afterwards.

Usage:
    python tools/sim_round.py --seconds 150 [--tag r1] [--keep] [--config config/config.ini]
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BIN = os.path.join(ROOT, "build", "bin")
LOGDIR = os.path.join(ROOT, "log")
PROCS = ["md_proc.exe", "trader_proc.exe", "query_proc.exe", "strategy_proc.exe"]
INITIAL_BALANCE = 1000000.0


def kill_all():
    subprocess.run(
        ["taskkill", "/F", "/T", "/IM", PROCS[0], "/IM", PROCS[1], "/IM", PROCS[2], "/IM", PROCS[3]],
        capture_output=True)


def start_all(cfg):
    procs = []
    for name in PROCS:
        p = subprocess.Popen(
            [os.path.join(BIN, name), "--config", cfg],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0),
        )
        procs.append(p)
        time.sleep(0.6)
    return procs


def wait_exit(procs, timeout=2.0):
    for p in procs:
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            pass


def parse_account_lines(path):
    """Return list of dicts parsed from 'account balance=...' log lines."""
    out = []
    if not os.path.exists(path):
        return out
    pat = re.compile(
        r"account balance=([\d.+-]+) available=[\d.+-]+ margin=[\d.+-]+ "
        r"realized=([\d.+-]+) fee=([\d.+-]+) pos=(\d+)")
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = pat.search(line)
            if m:
                out.append({
                    "balance": float(m.group(1)),
                    "realized": float(m.group(2)),
                    "fee": float(m.group(3)),
                    "positions": int(m.group(4)),
                })
    return out


def parse_round_trips(path):
    """Return list of pnl values from 'ROUND TRIP ... pnl=%+.2f/unit' lines."""
    out = []
    if not os.path.exists(path):
        return out
    pat = re.compile(r"ROUND TRIP \S+ closed @ ([\d.]+) \(entry=([\d.]+) (?:long|short), pnl=([+-][\d.]+)/unit")
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = pat.search(line)
            if m:
                out.append(float(m.group(3)))
    return out


def archive_logs(tag):
    dst = os.path.join(LOGDIR, "round_%s" % tag)
    if os.path.isdir(dst):
        shutil.rmtree(dst, ignore_errors=True)
    os.makedirs(dst, exist_ok=True)
    for name in ("md.log", "trader.log", "query.log", "strategy.log"):
        src = os.path.join(LOGDIR, name)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(dst, name))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=150)
    ap.add_argument("--tag", default="r")
    ap.add_argument("--config", default="config/config.ini")
    ap.add_argument("--keep", action="store_true", help="do not kill processes afterwards")
    ap.add_argument("--seed", type=int, default=None,
                    help="temporarily set [sim] seed for this round, restore afterwards")
    args = ap.parse_args()

    cfg_path = os.path.join(ROOT, args.config)
    orig_seed = None
    if args.seed is not None:
        with open(cfg_path, "r", encoding="utf-8", errors="replace") as f:
            cfg_text = f.read()
        m = re.search(r"^seed\s*=\s*(\S+)", cfg_text, re.M)
        orig_seed = m.group(1) if m else None
        if m:
            cfg_text = re.sub(r"^seed\s*=\s*\S+", "seed = %d" % args.seed, cfg_text, flags=re.M)
            with open(cfg_path, "w", encoding="utf-8", newline="") as f:
                f.write(cfg_text)

    kill_all()
    time.sleep(1.0)

    # fresh shared memory bus
    subprocess.run([os.path.join(BIN, "shm_reset.exe")], cwd=ROOT, capture_output=True)

    # fresh logs
    for name in ("md.log", "trader.log", "query.log", "strategy.log"):
        p = os.path.join(LOGDIR, name)
        if os.path.exists(p):
            try:
                os.remove(p)
            except OSError:
                pass

    try:
        procs = start_all(cfg_path)
        t0 = time.time()
        try:
            deadline = t0 + args.seconds
            while time.time() < deadline:
                alive = sum(1 for p in procs if p.poll() is None)
                if alive < 4:
                    print("[WARN] only %d/4 processes alive after %.0fs" % (alive, time.time() - t0),
                          file=sys.stderr)
                    break
                time.sleep(2.0)
        except KeyboardInterrupt:
            pass

        # give the query process a moment to log the last account line
        time.sleep(1.5)

        accounts = parse_account_lines(os.path.join(LOGDIR, "query.log"))
        trips = parse_round_trips(os.path.join(LOGDIR, "strategy.log"))
        last = accounts[-1] if accounts else None

        result = {
            "tag": args.tag,
            "seed": args.seed if args.seed is not None else (orig_seed or "fixed"),
            "seconds": int(time.time() - t0),
            "round_trips": len(trips),
            "gross_per_unit_sum": round(sum(trips), 2),
            "fee": round(last["fee"], 2) if last else 0.0,
            "realized": round(last["realized"], 2) if last else 0.0,
            "balance": round(last["balance"], 2) if last else None,
            "open_positions": last["positions"] if last else None,
            "net_pnl": round(last["balance"] - INITIAL_BALANCE, 2) if last else None,
            "account_lines": len(accounts),
        }
        print(json.dumps(result, ensure_ascii=False))

        if args.tag:
            archive_logs(args.tag)

        if not args.keep:
            kill_all()
            time.sleep(1.0)
        return 0
    finally:
        # restore the original seed
        if orig_seed is not None:
            with open(cfg_path, "r", encoding="utf-8", errors="replace") as f:
                cfg_text = f.read()
            if re.search(r"^seed\s*=\s*\S+", cfg_text, re.M):
                cfg_text = re.sub(r"^seed\s*=\s*\S+", "seed = %s" % orig_seed, cfg_text, flags=re.M)
                with open(cfg_path, "w", encoding="utf-8", newline="") as f:
                    f.write(cfg_text)


if __name__ == "__main__":
    sys.exit(main())
