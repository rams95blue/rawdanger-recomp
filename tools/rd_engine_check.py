#!/usr/bin/env python3
"""
rd_engine_check.py - what changed in ps2-native-recomp (upstream of engine/ps2-native-recomp) since our fork's base, sorted
by what it can do for Raw Danger. The same report as RawDangerPC.exe's engine update notice.

  python rd_engine_check.py [--out work/reports/engine_updates.md] [--json saved_compare.json]

Reads the base commit from engine/FORK.md ("Upstream base: commit `<sha>`"), asks GitHub's compare API (public, no
account; 60 requests an hour), sorts each change with launcher/engine_watch.txt, and names the files that upstream and our
patches (patches/native/*.patch) both change. Exit code 0; 1 when GitHub cannot be reached.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_rules():
    upstream, branch, skips, rules = "exoteric33/ps2-native-recomp", "main", [], []
    with open(os.path.join(ROOT, "launcher", "engine_watch.txt"), encoding="utf-8") as f:
        for raw in f:
            t = raw.strip()
            if not t or t.startswith("#") or "=" not in t:
                continue
            k, v = (x.strip() for x in t.split("=", 1))
            if k == "upstream": upstream = v
            elif k == "branch": branch = v
            elif k == "skip": skips.append(re.compile(v, re.I))
            elif k == "rule":
                area, weight, words = (x.strip() for x in v.split("|", 2))
                rules.append((area, int(weight), re.compile(words, re.I)))
    return upstream, branch, skips, rules


def base_commit():
    with open(os.path.join(ROOT, "engine", "FORK.md"), encoding="utf-8") as f:
        m = re.search(r"Upstream base: commit `([0-9a-f]{7,40})`", f.read())
    if not m:
        sys.exit("engine/FORK.md does not name the upstream base commit")
    return m.group(1)


def our_files():
    out = {}
    d = os.path.join(ROOT, "patches", "native")
    for name in sorted(os.listdir(d)):
        if not name.endswith(".patch"):
            continue
        with open(os.path.join(d, name), encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("diff --git "):
                    b = line.split()[-1]
                    for _ in range(2):
                        if b.startswith("b/"): b = b[2:]
                    out.setdefault(b, []).append(name.split("-")[0])
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(ROOT, "work", "reports", "engine_updates.md"))
    ap.add_argument("--json", help="a saved compare answer instead of asking GitHub")
    a = ap.parse_args(argv)
    upstream, branch, skips, rules = load_rules()
    base = base_commit()
    if a.json:
        with open(a.json, encoding="utf-8") as f:
            text = f.read()
        if not text.lstrip().startswith("{"):
            text = text.split("\n", 1)[1]   # the launcher's cache: base sha on the first line
        d = json.loads(text)
    else:
        url = f"https://api.github.com/repos/{upstream}/compare/{base}...{branch}?per_page=250"
        req = urllib.request.Request(url, headers={"User-Agent": "rawdanger-recomp rd_engine_check", "Accept": "application/vnd.github+json"})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                d = json.load(r)
        except Exception as e:
            print(f"could not reach GitHub: {e}")
            return 1
    changes, ignored = [], 0
    for c in reversed(d.get("commits", [])):
        title = c["commit"]["message"].split("\n")[0].strip()
        date = c["commit"]["committer"]["date"][:10]
        if any(s.search(title) for s in skips):
            ignored += 1
            continue
        hit = next(((area, w) for area, w, words in rules if words.search(title)), None)
        if not hit or hit[1] <= 0:
            ignored += 1
            continue
        changes.append((hit[1], date, hit[0], title, c.get("html_url", "")))
    points = sum(c[0] for c in changes)
    significant = any(c[0] >= 3 for c in changes) or points >= 8
    ours = our_files()
    clashes = [f"{f['filename']} ({', '.join(sorted(ours[f['filename']]))})" for f in d.get("files", []) if f["filename"] in ours]
    lines = [f"# Engine updates: {upstream} ({branch}) since {base[:7]}", "",
             f"{d.get('ahead_by', 0)} new changes upstream; {len(changes)} can help Raw Danger ({points} points), {ignored} do not concern it."
             f" Significant: {'yes' if significant else 'no'}.", ""]
    for area in [r[0] for r in rules]:
        rows = sorted((c for c in changes if c[2] == area), key=lambda c: c[1], reverse=True)
        if not rows:
            continue
        lines += [f"## {area} ({len(rows)})", ""]
        lines += [f"- {c[1]}  [{c[3]}]({c[4]})" for c in rows]
        lines.append("")
    lines += ["## Files changed on both sides", ""]
    lines += [f"- {c}" for c in clashes] or ["- none"]
    lines += ["", "Taking them in: a new upstream copy, `git apply` every patch in patches/native in order (fix the ones that",
              "clash), build one game, play-test, then update engine/FORK.md's base commit (see FORK.md, Updating from upstream).", ""]
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("\n".join(lines[:4]))
    print(f"report: {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
