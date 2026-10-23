#!/usr/bin/env python3
"""Publish a firmware blob to the Orange Pi OTA server (ota-firmware, :8080).

Usage:
    python ota_publish.py <firmware.bin> <version> [--device hioto-genmon] [--note "..."]

It uploads the blob over SFTP to /tmp on the Pi (192.168.1.99) and runs the
server's own publish_firmware.py with sudo, which computes size + SHA-256 and
updates /srv/ota/<device>/latest.json.
"""
import argparse
import json
import os
import sys
import urllib.request

import paramiko

HOST = "192.168.1.99"
USER = "orangepi"
PASS = "OrangePiUser1!"
PUBLISH = "/usr/local/bin/publish_firmware.py"


def ssh_run(client, cmd, timeout=120):
    stdin, stdout, stderr = client.exec_command(cmd, timeout=timeout, get_pty=True)
    out = stdout.read().decode(errors="replace")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("version")
    ap.add_argument("--device", default="hioto-genmon")
    ap.add_argument("--note", default="")
    a = ap.parse_args()

    if not os.path.isfile(a.file):
        sys.exit(f"not found: {a.file}")

    size = os.path.getsize(a.file)
    print(f"[publish] {a.file} ({size} bytes) -> {a.device} v{a.version}")

    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=PASS, timeout=15,
              look_for_keys=False, allow_agent=False)

    remote = f"/tmp/fw-{a.device}-{a.version}.bin"
    sftp = c.open_sftp()
    sftp.put(a.file, remote)
    sftp.close()
    print(f"[publish] uploaded to {remote}")

    note = a.note.replace('"', "'")
    cmd = (f'echo {PASS} | sudo -S python3 {PUBLISH} {remote} '
           f'--device {a.device} --version {a.version} --note "{note}" 2>&1')
    print(ssh_run(c, cmd))
    c.close()

    # read back the manifest through the public API
    url = f"http://{HOST}:8080/api/latest?device={a.device}&guid=publish-check"
    try:
        with urllib.request.urlopen(url, timeout=8) as r:
            data = json.load(r)
        print("[publish] manifest:", json.dumps(data, indent=2))
    except Exception as e:  # noqa: BLE001
        print("[publish] manifest read failed:", e)


if __name__ == "__main__":
    main()
