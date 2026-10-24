#!/usr/bin/env python3
"""Build, package and publish the HIOTO General Monitor firmwares.

Two targets, one code base - each board gets its own image because the display
controller differs, and each image is published to its own OTA channel so a
board can only ever pull the firmware for its own glass.

    genmon_35_st7796  ESP32-3248S035  3.5"  ST7796  480x320  hioto-genmon-35-st7796
    genmon_28_st7789  ESP32-2432S028  2.8"  ST7789  320x240  hioto-genmon-28-st7789

Usage:
    python ota_release.py                     # build both, package, do not publish
    python ota_release.py --publish           # build, package, publish to the Pi
    python ota_release.py --publish --no-build   # package + publish what is built
    python ota_release.py --board 35 --publish   # one target only

Packages land in firmware/releases/ as
    <type>-<version>.bin  plus MANIFEST.json and RELEASES.md
so the exact image that runs on a board is committed next to the source.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT / "firmware" / "hioto_general_monitor"
RELEASES = ROOT / "firmware" / "releases"
BUILD = PROJECT / ".pio" / "build"

# board -> (platformio env, board name, panel chip, OTA device type)
TARGETS = {
    "35": ("genmon_35_st7796", "ESP32-3248S035 (3.5\")", "ST7796", "hioto-genmon-35-st7796"),
    "28": ("genmon_28_st7789", "ESP32-2432S028 (2.8\")", "ST7789", "hioto-genmon-28-st7789"),
}

PIO = Path(os.environ.get("USERPROFILE", Path.home())) / ".platformio" / "penv" / "Scripts" / "pio.exe"
if not PIO.exists():
    PIO = Path("pio")  # fall back to PATH


def fw_version() -> str:
    for line in (PROJECT / "include" / "config.h").read_text(encoding="utf-8").splitlines():
        if line.strip().startswith("#define FW_VERSION"):
            return line.split('"')[1]
    sys.exit("FW_VERSION not found in include/config.h")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build(envs: list[str]) -> None:
    cmd = [str(PIO), "run"]
    for e in envs:
        cmd += ["-e", e]
    print(f"[release] {' '.join(cmd)}")
    r = subprocess.run(cmd, cwd=PROJECT)
    if r.returncode != 0:
        sys.exit("[release] build failed")


def package(board: str, version: str) -> dict:
    env, board_name, chip, dtype = TARGETS[board]
    src = BUILD / env / "firmware.bin"
    if not src.is_file():
        sys.exit(f"[release] missing build output: {src}")
    RELEASES.mkdir(parents=True, exist_ok=True)
    dst = RELEASES / f"{dtype}-{version}.bin"
    shutil.copy2(src, dst)
    entry = {
        "device_type": dtype,
        "board": board_name,
        "panel_chip": chip,
        "env": env,
        "version": version,
        "file": dst.name,
        "size": dst.stat().st_size,
        "sha256": sha256(dst),
        "built_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
    }
    print(f"[release] {dst.name}  {entry['size']} bytes  {entry['sha256'][:16]}...")
    return entry


def write_manifest(entries: list[dict], version: str) -> None:
    manifest_path = RELEASES / "MANIFEST.json"
    manifest = {"version": version, "releases": []}
    if manifest_path.is_file():
        try:
            old = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["releases"] = [
                r for r in old.get("releases", [])
                if r.get("device_type") not in {e["device_type"] for e in entries}
            ]
        except json.JSONDecodeError:
            pass
    manifest["releases"] += entries
    manifest["releases"].sort(key=lambda r: (r["device_type"], r["version"]))
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    lines = [
        "# HIOTO General Monitor — firmware releases",
        "",
        "Two boards, two images. The panel controller differs, so the images are",
        "not interchangeable: flashing the wrong one leaves the screen blank even",
        "though the board is healthy (the firmware prints a board-tag warning).",
        "",
        "| board | panel | OTA channel | env |",
        "|---|---|---|---|",
    ]
    for b in ("35", "28"):
        env, board_name, chip, dtype = TARGETS[b]
        lines.append(f"| {board_name} | {chip} | `{dtype}` | `{env}` |")
    lines += ["", "## Images", "", "| version | device type | chip | size | sha256 | file |",
              "|---|---|---|---|---|---|"]
    for r in manifest["releases"]:
        lines.append(
            f"| {r['version']} | `{r['device_type']}` | {r['panel_chip']} | "
            f"{r['size']} | `{r['sha256'][:16]}…` | `{r['file']}` |"
        )
    lines += [
        "",
        "Rebuild and publish both:",
        "",
        "```",
        "python ota_release.py --publish",
        "```",
        "",
        "Cable-flash one board (manual download mode: hold BOOT while powering):",
        "",
        "```",
        f"python -m platformio run -e genmon_35_st7796 -t upload",
        "```",
    ]
    (RELEASES / "RELEASES.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"[release] wrote {manifest_path.name} and RELEASES.md")


def publish(entry: dict, note: str) -> None:
    cmd = [sys.executable, str(ROOT / "ota_publish.py"), str(RELEASES / entry["file"]),
           entry["version"], "--device", entry["device_type"], "--note", note]
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit(f"[release] publish failed for {entry['device_type']}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", choices=["35", "28", "both"], default="both")
    ap.add_argument("--no-build", action="store_true", help="package what is already built")
    ap.add_argument("--publish", action="store_true", help="push to the Orange Pi OTA server")
    ap.add_argument("--note", default="")
    a = ap.parse_args()

    boards = ["35", "28"] if a.board == "both" else [a.board]
    version = fw_version()

    if not a.no_build:
        build([TARGETS[b][0] for b in boards])

    entries = [package(b, version) for b in boards]
    write_manifest(entries, version)

    if a.publish:
        for e in entries:
            note = a.note or f"{e['panel_chip']} build {e['version']} for {e['board']}"
            publish(e, note)
    else:
        print("[release] not published (pass --publish)")

    print(json.dumps(entries, indent=2))


if __name__ == "__main__":
    main()
