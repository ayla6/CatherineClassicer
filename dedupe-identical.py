#!/usr/bin/env python3
"""Reflink byte-identical files under <game>/data so btrfs stores them once.

Covers movie vs movie_jp, sound vs sound_jp, and misc duplicate assets.
Nothing is deleted; each duplicate is replaced by a reflink of the first copy.

usage: dedupe-identical.py [--dry-run] [GAME_DIR]
"""
import argparse, collections, filecmp, hashlib, os, subprocess, sys

DEFAULT = "/var/data/Games/common/Catherine Classic"
EXCLUDE_DIRS = {"test"}   # data/test is left alone on purpose
MIN_SIZE = 4096           # smaller files can't share a btrfs block anyway


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game", nargs="?", default=DEFAULT)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    data = os.path.join(a.game, "data")
    if not os.path.isdir(data):
        sys.exit(f"not found: {data}")

    by_size = collections.defaultdict(list)
    for root, dirs, files in os.walk(data):
        if root == data:
            dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
        for f in files:
            p = os.path.join(root, f)
            if not os.path.islink(p) and os.path.getsize(p) >= MIN_SIZE:
                by_size[os.path.getsize(p)].append(p)

    n = saved = 0
    for size, paths in by_size.items():
        if len(paths) < 2:
            continue
        by_hash = collections.defaultdict(list)
        for p in paths:
            by_hash[sha1(p)].append(p)
        for group in by_hash.values():
            src = sorted(group)[0]   # sorted -> 'movie' wins over 'movie_jp'
            for dst in sorted(group)[1:]:
                if not filecmp.cmp(src, dst, shallow=False):
                    continue
                print(f"{'would link' if a.dry_run else 'link'}: {dst} <- {src}")
                if not a.dry_run:
                    subprocess.run(["cp", "--reflink=always", "--preserve=all", src, dst], check=True)
                n += 1
                saved += size
    print(f"{n} files, {saved / 2**20:.0f} MiB shareable")


if __name__ == "__main__":
    main()
