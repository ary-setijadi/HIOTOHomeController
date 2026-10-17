#!/usr/bin/env python3
"""Diagnose a failed flash verification.

Answers, in order:
  1. did ANY of the image land? (look for the Allwinner eGON signature at 0x2004)
  2. is the card's content STABLE between two independent read passes?
     (an unstable read means the card or reader is the problem, not the write)
  3. exactly which regions differ from the source image?

Read-only: never writes to the card.

Usage:  python diagnose_flash.py <image> [--disk 1]
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys
import time

CHUNK = 1 << 20  # 1 MiB


def read_exact(fh, offset: int, size: int) -> bytes:
    fh.seek(offset)
    buf = b""
    while len(buf) < size:
        b = fh.read(size - len(buf))
        if not b:
            break
        buf += b
    return buf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--disk", type=int, default=1)
    ap.add_argument("--max-report", type=int, default=25)
    args = ap.parse_args()

    disk_path = r"\\.\PhysicalDrive%d" % args.disk
    img_size = os.path.getsize(args.image)
    print(f"image : {args.image}")
    print(f"        {img_size:,} bytes ({img_size/2**30:.2f} GiB)")
    print(f"card  : {disk_path}")

    img = open(args.image, "rb")

    # ---- 1. signature check on the first 64 KiB -------------------------------
    card = open(disk_path, "rb", buffering=0)
    try:
        c_head = read_exact(card, 0, 65536)
        i_head = read_exact(img, 0, 65536)
    finally:
        card.close()

    for name, blob in (("image", i_head), ("card ", c_head)):
        egon = blob.find(b"eGON")
        mbr = blob[510:512]
        print(f"  {name}: mbr_sig={mbr.hex()}  'eGON' at {hex(egon) if egon >= 0 else 'NOT FOUND'}")
    print(f"  first 64KiB identical: {c_head == i_head}")

    # ---- 2 + 3. two full read passes, hashing AND comparing -------------------
    hashes = []
    mismatches = []
    first_diff = None
    for p in (1, 2):
        t0 = time.time()
        h = hashlib.sha256()
        mism = []
        fd = open(disk_path, "rb", buffering=0)
        img.seek(0)
        off = 0
        try:
            while off < img_size:
                want = min(CHUNK, img_size - off)
                # raw device reads may return short -- loop until satisfied
                cbuf = b""
                while len(cbuf) < want:
                    b = fd.read(want - len(cbuf))
                    if not b:
                        break
                    cbuf += b
                if len(cbuf) != want:
                    print(f"  pass{p}: SHORT READ at {off:,}: got {len(cbuf)} of {want}")
                    break
                ibuf = b""
                while len(ibuf) < want:
                    b = img.read(want - len(ibuf))
                    if not b:
                        break
                    ibuf += b
                h.update(cbuf)
                if p == 1 and cbuf != ibuf:
                    mism.append(off)
                    if first_diff is None:
                        for k in range(want):
                            if cbuf[k] != ibuf[k]:
                                first_diff = (off + k, ibuf[k], cbuf[k])
                                break
                off += want
        finally:
            fd.close()
        hashes.append(h.hexdigest())
        if p == 1:
            mismatches = mism
        print(f"  pass{p}: {off:,} bytes in {time.time()-t0:.1f}s  sha256={h.hexdigest()}")
        if p == 1:
            img.seek(0)

    img.close()

    print()
    print("== STABILITY ==")
    if hashes[0] == hashes[1]:
        print("  two independent read passes agree -> the card's content is STABLE")
        print("  => this is a WRITE problem, not an unstable card/reader")
    else:
        print("  *** two read passes DISAGREE -> the card or reader is UNSTABLE ***")
        print("  => matches the documented flaky-reader failure; a reboot may help")

    print()
    print("== DIFF VS IMAGE ==")
    total_chunks = (img_size + CHUNK - 1) // CHUNK
    print(f"  mismatching 1 MiB chunks: {len(mismatches)} of {total_chunks}")
    if first_diff:
        print(f"  first differing byte at offset {first_diff[0]:,}: "
              f"image=0x{first_diff[1]:02x} card=0x{first_diff[2]:02x}")
    for m in mismatches[: args.max_report]:
        print(f"    mismatch @ {m:,} ({m/2**20:.1f} MiB)")
    if len(mismatches) > args.max_report:
        print(f"    ... and {len(mismatches) - args.max_report} more")

    # A card that is mostly blank/zereos where the image is not tells a different
    # story from one with shifted or random data.
    return 0


if __name__ == "__main__":
    sys.exit(main())
