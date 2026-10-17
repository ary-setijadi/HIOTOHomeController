#!/usr/bin/env python3
"""Re-read the card several times and report per-pass divergence from the image.

Distinguishes two very different situations:

  * reads vary between passes  -> flaky READ path (reader/cable/card), so the
    bytes on the card may well be fine; the documented remedy is a host reboot
    (SETUP-OrangePi-PiHole-OTA.md lesson 1: "A PC reboot fixed the reader").
  * reads are stable but differ from the image at fixed offsets -> the WRITE
    genuinely did not stick there.

Also hammers the suspect offsets to count how often they read incorrectly.

Read-only: never writes to the card.

Usage: python recheck_card.py <image> [--disk 1] [--passes 3]
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys
import time

CHUNK = 1 << 20


def read_exact(fh, offset, size):
    fh.seek(offset)
    buf = b""
    while len(buf) < size:
        b = fh.read(size - len(buf))
        if not b:
            break
        buf += b
    return buf


def read_chunk(fd, offset, size):
    fd.seek(offset)
    buf = b""
    while len(buf) < size:
        b = fd.read(size - len(buf))
        if not b:
            break
        buf += b
    return buf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--disk", type=int, default=1)
    ap.add_argument("--passes", type=int, default=3)
    ap.add_argument("--suspects", default="4194304,5242880")
    ap.add_argument("--hammer", type=int, default=15)
    args = ap.parse_args()

    disk_path = r"\\.\PhysicalDrive%d" % args.disk
    img_size = os.path.getsize(args.image)
    suspects = [int(x) for x in args.suspects.split(",") if x.strip()]
    img = open(args.image, "rb")

    print(f"image {img_size:,} bytes; {args.passes} full read passes\n")
    pass_hashes = []
    pass_mismatch_sets = []
    for p in range(1, args.passes + 1):
        t0 = time.time()
        h = hashlib.sha256()
        mism = []
        fd = open(disk_path, "rb", buffering=0)
        try:
            off = 0
            while off < img_size:
                want = min(CHUNK, img_size - off)
                cbuf = read_chunk(fd, off, want)
                if len(cbuf) != want:
                    print(f"  pass{p}: SHORT READ at {off:,}")
                    break
                ibuf = read_exact(img, off, want)
                h.update(cbuf)
                if cbuf != ibuf:
                    mism.append(off)
                off += want
        finally:
            fd.close()
        pass_hashes.append(h.hexdigest())
        pass_mismatch_sets.append(set(mism))
        print(f"pass{p}: {time.time()-t0:5.1f}s  sha256={h.hexdigest()}")
        print(f"        mismatching chunks: {len(mism)}  {[hex(m) for m in mism[:10]]}")

    print("\n== STABILITY ==")
    uniq = len(set(pass_hashes))
    print(f"  distinct hashes across {args.passes} passes: {uniq}")
    if uniq == 1:
        print("  reads are STABLE")
    else:
        print("  *** reads VARY between passes -> flaky read path (reader/card) ***")

    always = set.intersection(*pass_mismatch_sets) if pass_mismatch_sets else set()
    ever = set.union(*pass_mismatch_sets) if pass_mismatch_sets else set()
    print(f"  chunks wrong in EVERY pass : {sorted(hex(x) for x in always)}")
    print(f"  chunks wrong in SOME pass  : {sorted(hex(x) for x in ever)}")

    print("\n== SUSPECT REGION HAMMER ==")
    for s in suspects:
        want = min(CHUNK, img_size - s)
        ibuf = read_exact(img, s, want)
        good_hash = hashlib.sha256(ibuf).hexdigest()
        seen = {}
        fd = open(disk_path, "rb", buffering=0)
        try:
            for _ in range(args.hammer):
                cbuf = read_chunk(fd, s, want)
                hh = hashlib.sha256(cbuf).hexdigest()
                seen[hh] = seen.get(hh, 0) + 1
        finally:
            fd.close()
        print(f"  offset {s:,} ({s/2**20:.0f} MiB): {len(seen)} distinct contents in {args.hammer} reads")
        for hh, n in sorted(seen.items(), key=lambda kv: -kv[1]):
            tag = "  <-- MATCHES IMAGE" if hh == good_hash else ""
            print(f"      {hh[:32]}...  x{n}{tag}")
    img.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
