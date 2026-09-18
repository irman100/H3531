#!/usr/bin/env python3
"""Resolve and download a Debian Wheezy armel runtime without executing ARM code.

Inputs are Debian Archive Packages.gz files. The resolver follows Pre-Depends
and Depends recursively, choosing the first available alternative. Security
records loaded later override the base archive records for the same package.
"""
import argparse, gzip, os, re, urllib.request
from pathlib import Path

def parse_packages(path, base_url, db):
    raw = gzip.open(path, "rt", encoding="utf-8", errors="replace").read()
    for stanza in raw.split("\n\n"):
        fields = {}
        key = None
        for line in stanza.splitlines():
            if not line:
                continue
            if line[0].isspace() and key:
                fields[key] += " " + line.strip()
                continue
            if ": " in line:
                key, val = line.split(": ", 1)
                fields[key] = val
        name = fields.get("Package")
        fn = fields.get("Filename")
        if name and fn:
            fields["_base"] = base_url.rstrip("/")
            db[name] = fields

def dep_name(expr):
    expr = re.sub(r"\([^)]*\)", "", expr).strip()
    expr = re.sub(r"\[[^]]*\]", "", expr).strip()
    expr = expr.split()[0] if expr else ""
    expr = expr.split(":", 1)[0]
    return expr

def resolve(roots, db):
    wanted, queue = set(), list(roots)
    while queue:
        name = queue.pop(0)
        if name in wanted:
            continue
        rec = db.get(name)
        if rec is None:
            raise SystemExit(f"Missing package in Wheezy indexes: {name}")
        wanted.add(name)
        for field in ("Pre-Depends", "Depends"):
            value = rec.get(field, "")
            if not value:
                continue
            for group in value.split(","):
                candidates = [dep_name(x) for x in group.split("|")]
                chosen = next((x for x in candidates if x in db), None)
                if chosen and chosen not in wanted:
                    queue.append(chosen)
    return sorted(wanted)

def download(url, dst):
    req = urllib.request.Request(url, headers={"User-Agent": "H3531-stage6-builder/1.0"})
    with urllib.request.urlopen(req, timeout=120) as r, open(dst, "wb") as f:
        while True:
            b = r.read(1024 * 1024)
            if not b:
                break
            f.write(b)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main-index", required=True)
    ap.add_argument("--security-index", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("roots", nargs="+")
    a = ap.parse_args()

    db = {}
    parse_packages(a.main_index, "https://archive.debian.org/debian", db)
    parse_packages(a.security_index, "https://archive.debian.org/debian-security", db)
    wanted = resolve(a.roots, db)

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    manifest = []
    for i, name in enumerate(wanted, 1):
        rec = db[name]
        fn = rec["Filename"]
        url = rec["_base"] + "/" + fn
        dst = out / os.path.basename(fn)
        print(f"[{i}/{len(wanted)}] {name} {rec.get('Version','')} -> {dst.name}", flush=True)
        if not dst.exists():
            download(url, dst)
        manifest.append(
            f"{name}\t{rec.get('Version','')}\t{rec.get('Architecture','')}\t{url}\t{dst.name}"
        )

    (out / "PACKAGES.tsv").write_text("\n".join(manifest) + "\n", encoding="utf-8")
    print(f"Resolved {len(wanted)} packages")

if __name__ == "__main__":
    main()
