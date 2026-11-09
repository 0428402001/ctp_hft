#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""tailf.py - a `tail -f` that reliably works in a Windows console.

Why this exists: the uutils tail.exe that watch_log.bat used printed the
initial lines and then went silent when attached to a real console (its
follow/flush behaviour there is unreliable), so the log view "did not
refresh".  This script owns the whole path: it reads the file in binary,
polls every 0.3 s, decodes each line (UTF-8 first, GBK fallback - trader.log
carries GBK text from the CTP error messages) and prints with an explicit
flush, which is correct on every console.

Usage:
    python tools/tailf.py [-n N] [--grep REGEX] FILE [FILE ...]

    -n N        lines of history to show first (default 30)
    --grep R    only print lines matching regex R (used by "errors" mode)
    multiple FILEs -> every line is prefixed with [name] tags

Ctrl+C stops it.  It never touches the trading processes.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import time

POLL_S = 0.3


def decode(line: bytes) -> str:
    for enc in ("utf-8", "gbk"):
        try:
            return line.decode(enc)
        except UnicodeDecodeError:
            continue
    return line.decode("utf-8", "replace")


class Follower:
    """Follows one file; yields decoded lines as they are appended."""

    def __init__(self, path: str):
        self.path = path
        self.pos = 0
        self.partial = b""
        self.tag = os.path.splitext(os.path.basename(path))[0]

    def _read(self, size_hint: int = 1 << 20) -> bytes:
        try:
            with open(self.path, "rb") as f:
                f.seek(self.pos)
                data = f.read(size_hint)
        except OSError:
            return b""
        self.pos += len(data)
        return data

    def _check_truncated(self) -> bool:
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return False
        if size < self.pos:            # log got truncated/recreated
            self.pos = 0
            self.partial = b""
            return True
        return False

    def history(self, n: int) -> list[str]:
        """Last n lines without loading the whole (multi-GB) file."""
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return []
        chunk = 64 * 1024
        out: list[bytes] = []
        with open(self.path, "rb") as f:
            while len(out) <= n and size > 0:
                start = max(size - chunk, 0)
                f.seek(start)
                blob = f.read(size - start)
                lines = blob.split(b"\n")
                if start > 0:
                    lines = lines[1:]          # first is a partial line
                out = lines + out
                if start == 0:
                    break
                size = start
                chunk *= 2
        self.pos = os.path.getsize(self.path)
        self.partial = b""
        return [decode(l) for l in out[-n:] if l != b""]

    def poll(self) -> list[str]:
        if self._check_truncated():
            return [f"--- {self.tag}.log was truncated, following from the start ---"]
        data = self._read()
        if not data:
            return []
        lines = (self.partial + data).split(b"\n")
        self.partial = lines.pop()             # keep the unterminated tail
        return [decode(l) for l in lines if l != b""]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=30, help="history lines")
    ap.add_argument("--grep", default=None, help="only lines matching this regex")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()

    flt = re.compile(args.grep) if args.grep else None
    multi = len(args.files) > 1
    followers = [Follower(p) for p in args.files]

    def emit(tag: str, line: str) -> None:
        if flt is not None and not flt.search(line):
            return
        print(f"[{tag}] {line}" if multi else line, flush=True)

    for fw in followers:
        for line in fw.history(args.n):
            emit(fw.tag, line)

    try:
        while True:
            moved = False
            for fw in followers:
                for line in fw.poll():
                    emit(fw.tag, line)
                    moved = True
            if not moved:
                time.sleep(POLL_S)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
