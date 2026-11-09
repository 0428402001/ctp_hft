#!/usr/bin/env python3
"""Fetch a portable MinGW-w64 toolchain (no admin rights required).

Only needed so the framework can be compiled/verified on machines without
Visual Studio; the production build on Windows still uses MSVC (see build.bat).
"""
import os
import sys
import subprocess
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, ".toolchain")
RAW_URL = ("https://github.com/brechtsanders/winlibs_mingw/releases/download/"
           "16.2.0posix-23.1.3-14.0.0-ucrt-r3/winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r3.7z")
# Public mirrors are tried first: direct access to objects.githubusercontent.com
# is blocked on many corporate networks.
MIRRORS = ["https://ghfast.top/", "https://ghproxy.net/", "https://gh-proxy.com/", ""]


def main():
    os.makedirs(DEST, exist_ok=True)
    archive = os.path.join(DEST, "mingw.7z")

    if not os.path.exists(archive):
        done = False
        for prefix in MIRRORS:
            url = prefix + RAW_URL
            print("trying", url[:60], flush=True)
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "curl"})
                with urllib.request.urlopen(req, timeout=300) as r, open(archive, "wb") as f:
                    total = 0
                    while True:
                        chunk = r.read(1 << 20)
                        if not chunk:
                            break
                        f.write(chunk)
                        total += len(chunk)
                        print("\r  %d MB" % (total // 1024 // 1024), end="", flush=True)
                print("\ndownload done", flush=True)
                done = True
                break
            except Exception as exc:  # try next mirror
                print("  failed:", str(exc)[:80], flush=True)
                if os.path.exists(archive):
                    os.remove(archive)
        if not done:
            print("all mirrors failed", flush=True)
            return 1

    try:
        import py7zr  # noqa
    except ImportError:
        print("installing py7zr", flush=True)
        subprocess.check_call([sys.executable, "-m", "pip", "install", "-q", "py7zr"])

    import py7zr
    print("extracting (this takes a while)...", flush=True)
    with py7zr.SevenZipFile(archive) as z:
        z.extractall(path=DEST)
    print("extracted to", DEST, flush=True)

    gpp = None
    for dirpath, _dirs, files in os.walk(DEST):
        if "g++.exe" in files:
            gpp = os.path.join(dirpath, "g++.exe")
            break
    print("GPP=" + (gpp or "NOT FOUND"), flush=True)


if __name__ == "__main__":
    main()
