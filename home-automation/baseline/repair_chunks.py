#!/usr/bin/env python3
"""Resilient flash repair: rewrite only the chunks that do not match, then re-verify.

Purpose: separate "the write did not stick once" from "these cells cannot be
written at all".

  * if a mismatching chunk matches after ONE rewrite -> the media is fine and the
    original write hit a transient fault (flaky reader/controller, as documented
    for this reader in SETUP-OrangePi-PiHole-OTA.md lesson 1).
  * if a chunk still mismatches after several rewrites -> those blocks are bad
    and the card must be replaced.

Writes only the chunks that differ.

Usage: python repair_chunks.py <image> [--disk 1] [--attempts 4]
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


def compare_pass(disk_path, img, img_size):
    """Return (hash, [mismatching offsets], elapsed)."""
    t0 = time.time()
    h = hashlib.sha256()
    mism = []
    fd = open(disk_path, "rb", buffering=0)
    try:
        off = 0
        while off < img_size:
            want = min(CHUNK, img_size - off)
            cbuf = read_exact(fd, off, want)
            if len(cbuf) != want:
                raise SystemExit(f"short read at {off:,}")
            h.update(cbuf)
            if cbuf != read_exact(img, off, want):
                mism.append(off)
            off += want
    finally:
        fd.close()
    return h.hexdigest(), mism, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--disk", type=int, default=1)
    ap.add_argument("--attempts", type=int, default=4)
    args = ap.parse_args()

    disk_path = r"\\.\PhysicalDrive%d" % args.disk
    img_size = os.path.getsize(args.image)
    img = open(args.image, "rb")

    field = {}
    for name, off, size in (("zero", 0, 4096), ("egon", 0x2000, 4096), ("p1", 0x400000, 4096), ("tail", img_size - 4096, 4096)):
        field[name] = read_exact(img, off, size)

    history = []
    for attempt in range(1, args.attempts + 1):
        print(f"===== round {attempt}: verifying")
        ch, mism, el = compare_pass(disk_path, img, img_size)
        print(f"  sha256={ch}  mismatching chunks={len(mism)}  ({el:.1f}s)")
        for m in mism[:12]:
            print(f"    @ {m:,} ({m/2**20:.0f} MiB)")
        if len(mism) > 12:
            print(f"    ... and {len(mism)-12} more")
        history.append((attempt, ch, len(mism)))

        if not mism:
            print("\n*** CARD NOW MATCHES THE IMAGE EXACTLY ***")
            break

        print(f"  rewriting {len(mism)} chunk(s) ...")
        wfd = open(disk_path, "r+b", buffering=0)
        try:
            for m in mism:
                want = min(CHUNK, img_size - m)
                wfd.seek(m)
                wfd.write(read_exact(img, m, want))
                wfd.flush()
                os.fsync(wfd.fileno())
        finally:
            wfd.close()
        time.sleep(1)

    print("\n== HISTORY ==")
    for a, ch, n in history:
        print(f"  round {a}: {n:5d} mismatching chunks  {ch}")

    print("\n== FIELD SPOT CHECKS (what is actually on the card now) ==")
    fd = open(disk_path, "rb", buffering=0)
    try:
        for name, off, size in (("zero", 0, 4096), ("egon", 0x2000, 4096), ("p1", 0x400000, 4096), ("tail", img_size - 4096, 4096)):
            got = read_exact(fd, off, size)
            print(f"  {name:5s} @0x{off:08x}: {'MATCH' if got == field[name] else 'MISMATCH'}  "
                  f"img={field[name][:8].hex()} card={got[:8].hex()}")
    finally:
        fd.close()
    img.close()

    last = history[-1][2]
    if last == 0:
        print("\nRESULT: card verified good")
        return 0
    print(f"\nRESULT: card still has {last} bad chunk(s) after {len(history)} round(s) -> suspect failing media")
    return 1


if __name__ == "__main__":
    sys.exit(main())
