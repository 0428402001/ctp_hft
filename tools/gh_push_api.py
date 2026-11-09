#!/usr/bin/env python3
"""Push the current HEAD to GitHub through the REST git-data API.

Why this exists instead of `git push`
-------------------------------------
On the dev box this repo lives on, DNS resolves github.com to an edge IP that
is blackholed on :443 (TCP connects, TLS handshake times out), while
api.github.com answers in ~0.5s. So `git push` cannot work there at all, but
the API can carry the whole commit: blobs -> tree -> commit -> ref.

What it does
------------
1. reads HEAD (`ls-tree -r`, `cat-file`) with *git* semantics, so blob bytes and
   file modes are exactly what the index holds -- reading the working tree
   instead would silently re-encode GBK sources and lose the +x bits;
2. refuses to continue if any uploaded blob changes sha (content check);
3. recreates the tree, then a commit with HEAD's parents / author / committer /
   message, so the resulting sha is normally identical to local HEAD;
4. creates or force-updates the branch ref, then verifies the remote tree.

Usage
-----
    set GITHUB_TOKEN=ghp_xxx            # needs `repo` scope
    python tools\\gh_push_api.py
    python tools\\gh_push_api.py --repo D:\\quant\\ctp_hft --target 0428402001/ctp_hft --branch main

Force-update is intentional: it is used to repair a branch that was pushed out
of band, and every push is verified against the local tree afterwards.
"""
import argparse
import base64
import concurrent.futures as cf
import datetime as dt
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

API = "https://api.github.com"
# machine-wide http(s)_proxy answers 502 for github hosts; api.github.com is fine direct
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def api(method, path, token, payload=None):
    """Call the REST API with small exponential backoff on transient errors."""
    data = json.dumps(payload).encode() if payload is not None else None
    for attempt in range(4):
        req = urllib.request.Request(API + path, data=data, method=method)
        req.add_header("Authorization", "Bearer " + token)
        req.add_header("Accept", "application/vnd.github+json")
        req.add_header("X-GitHub-Api-Version", "2022-11-28")
        req.add_header("User-Agent", "ctp_hft-push")
        if data:
            req.add_header("Content-Type", "application/json")
        try:
            with OPENER.open(req, timeout=60) as resp:
                body = resp.read()
                return json.loads(body) if body else {}
        except urllib.error.HTTPError as e:
            detail = e.read().decode("utf-8", "replace")[:400]
            if e.code in (429, 500, 502, 503) and attempt < 3:
                time.sleep(2 * (attempt + 1))
                continue
            raise RuntimeError(f"{method} {path} -> {e.code}\n{detail}") from None
        except (urllib.error.URLError, TimeoutError):
            if attempt < 3:
                time.sleep(2 * (attempt + 1))
                continue
            raise
    raise RuntimeError("unreachable")


def git(repo, *args):
    return subprocess.run(["git", "-C", repo, *args], capture_output=True, check=True).stdout


def head_entries(repo):
    """[(mode, blob_sha, path)] for every file in HEAD."""
    out = git(repo, "ls-tree", "-r", "-z", "HEAD")
    items = []
    for rec in out.split(b"\0"):
        if not rec:
            continue
        meta, path = rec.split(b"\t", 1)
        mode, typ, sha = meta.decode().split()
        if typ != "blob":  # submodule / symlink-to-dir: not expected in this repo
            raise SystemExit(f"unsupported tree entry {typ}: {path!r}")
        items.append((mode, sha, path.decode("utf-8")))
    return items


def read_objects(repo, shas):
    """sha -> raw bytes, one `cat-file --batch` round trip."""
    proc = subprocess.Popen(["git", "-C", repo, "cat-file", "--batch"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    out, _ = proc.communicate(("\n".join(shas) + "\n").encode())
    objs, pos = {}, 0
    while pos < len(out):
        nl = out.index(b"\n", pos)
        header = out[pos:nl].decode()
        if header.endswith((" missing", " ambiguous")):
            raise SystemExit(f"git: {header}")
        sha, _typ, size = header.split()
        size = int(size)
        objs[sha] = out[nl + 1:nl + 1 + size]
        pos = nl + 1 + size + 1
    return objs


def head_commit_meta(repo):
    raw = git(repo, "cat-file", "-p", "HEAD")
    headers, _, message = raw.partition(b"\n\n")
    parents, author, committer = [], None, None
    for line in headers.decode("utf-8", "replace").splitlines():
        if line.startswith("parent "):
            parents.append(line.split()[1])
        elif line.startswith("author "):
            author = line
        elif line.startswith("committer "):
            committer = line
    return parents, author, committer, message.decode("utf-8")


def ident_from(line):
    """`author Name <email> 1700000000 +0800` -> GitHub's author/committer object."""
    line = line.split(" ", 1)[1]  # drop the `author` / `committer` keyword
    head, _, tail = line.rpartition("> ")
    name, _, email = head.partition(" <")
    stamp, _, offset = tail.partition(" ")
    sign = 1 if offset.startswith("+") else -1
    tz = dt.timezone(sign * dt.timedelta(hours=int(offset[1:3]), minutes=int(offset[3:5])))
    return {"name": name, "email": email,
            "date": dt.datetime.fromtimestamp(int(stamp), tz).isoformat()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--target", default="0428402001/ctp_hft")
    ap.add_argument("--branch", default="main")
    args = ap.parse_args()

    token = os.environ.get("GITHUB_TOKEN", "").strip()
    if not token:
        raise SystemExit("GITHUB_TOKEN not set (classic token with `repo` scope)")

    repo, target, branch = args.repo, args.target, args.branch
    me = api("GET", "/user", token)
    print(f"auth ok: {me['login']}")

    entries = head_entries(repo)
    shas = sorted({e[1] for e in entries})
    objs = read_objects(repo, shas)
    print(f"{len(entries)} paths / {len(shas)} blobs")
    if len(objs) != len(shas):
        raise SystemExit("some blobs could not be read from the object store")

    def upload(sha):
        payload = {"content": base64.b64encode(objs[sha]).decode(), "encoding": "base64"}
        return sha, api("POST", f"/repos/{target}/git/blobs", token, payload)["sha"]

    new_sha = {}
    with cf.ThreadPoolExecutor(max_workers=8) as pool:
        for i, (old, new) in enumerate(pool.map(upload, shas), 1):
            new_sha[old] = new
            if i % 25 == 0 or i == len(shas):
                print(f"  blobs {i}/{len(shas)}")

    changed = [s for s, n in new_sha.items() if s != n]
    if changed:
        raise SystemExit(f"content changed on {len(changed)} blob(s) -- aborting before touching the branch")
    print("all blob shas preserved byte-for-byte")

    tree = api("POST", f"/repos/{target}/git/trees", token,
               {"tree": [{"path": p, "mode": m, "type": "blob", "sha": new_sha[s]}
                         for m, s, p in entries]})["sha"]
    local_tree = git(repo, "rev-parse", "HEAD^{tree}").decode().strip()
    print(f"tree {tree[:12]} match={tree == local_tree}")
    if tree != local_tree:
        raise SystemExit("tree mismatch -- aborting before touching the branch")

    parents, author, committer, message = head_commit_meta(repo)
    payload = {"message": message, "tree": tree,
               "author": ident_from(author), "committer": ident_from(committer)}
    if parents:
        payload["parents"] = parents
    commit = api("POST", f"/repos/{target}/git/commits", token, payload)["sha"]
    local_commit = git(repo, "rev-parse", "HEAD").decode().strip()
    print(f"commit {commit[:12]} match={commit == local_commit}")

    try:
        api("POST", f"/repos/{target}/git/refs", token,
            {"ref": f"refs/heads/{branch}", "sha": commit})
        print(f"ref created: {branch}")
    except RuntimeError as e:
        if "422" not in str(e):
            raise
        api("PATCH", f"/repos/{target}/git/refs/heads/{branch}", token,
            {"sha": commit, "force": True})
        print(f"ref updated: {branch}")

    check = api("GET", f"/repos/{target}/git/trees/{branch}?recursive=1", token)
    print(f"remote {branch}: {len(check['tree'])} entries, truncated={check.get('truncated')}")
    if commit != local_commit:
        print("NOTE: remote sha != local HEAD (usually a parent/date difference); "
              "run `git fetch` on a machine that can reach github.com before the next push")
    return 0


if __name__ == "__main__":
    sys.exit(main())
