#!/usr/bin/env python3
"""Sanity-check a rendered page screenshot without displaying it.

Confirms the page actually painted: the dark application background dominates,
text pixels exist, and the palette contains the expected accent colours (so we
are not looking at a blank page, an error page, or a failed stylesheet).
"""
import sys
from collections import Counter

from PIL import Image

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

BG = (14, 21, 36)          # --bg
PANEL = (22, 32, 47)       # --panel
ACCENTS = {
    "green": (46, 204, 113),
    "blue": (74, 163, 255),
    "orange": (255, 180, 84),
    "teal": (45, 212, 191),
    "purple": (167, 139, 250),
}


def near(c1, c2, tol=26):
    return all(abs(a - b) <= tol for a, b in zip(c1, c2))


def main(path):
    img = Image.open(path).convert("RGB")
    w, h = img.size
    print(f"image      : {w}x{h}")

    pixels = list(img.getdata())
    total = len(pixels)
    counts = Counter(pixels)

    bg_hits = sum(n for c, n in counts.items() if near(c, BG))
    panel_hits = sum(n for c, n in counts.items() if near(c, PANEL))
    text_hits = sum(n for c, n in counts.items()
                    if sum(c) > 430 and not near(c, (255, 255, 255), 6))
    white_hits = sum(n for c, n in counts.items() if sum(c) > 700)

    print(f"dark bg    : {bg_hits/total*100:5.1f}%  (expect a large majority)")
    print(f"panel fill : {panel_hits/total*100:5.1f}%")
    print(f"text px    : {text_hits/total*100:5.2f}%")
    print(f"bright px  : {white_hits/total*100:5.2f}%")

    print("accents    :")
    for name, rgb in ACCENTS.items():
        hits = sum(n for c, n in counts.items() if near(c, rgb, 34))
        mark = "present" if hits > 40 else "absent"
        print(f"  {name:<7} {mark}  ({hits} px)")

    ok = True
    if not (bg_hits / total > 0.55):
        print("!! background does not dominate -> page may not have rendered")
        ok = False
    if text_hits < total * 0.002:
        print("!! almost no text pixels -> content missing")
        ok = False
    distinct = len(counts)
    print(f"distinct   : {distinct} colours")
    if distinct < 60:
        print("!! too few colours -> flat/blank render")
        ok = False
    print("RESULT     :", "looks painted" if ok else "SUSPECT")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "_shot_overview.png"))
