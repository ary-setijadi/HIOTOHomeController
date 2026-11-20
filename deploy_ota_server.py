#!/usr/bin/env python3
"""Deploy ota_server.py to the Orange Pi and restart the ota-firmware service.

Usage:
    python deploy_ota_server.py [--no-restart]

Steps: back up the live file, upload the workspace copy over SFTP, verify the
syntax with the target interpreter, swap it in as root, restart the unit, then
probe the API and show the service state. Nothing touches /srv/ota, so stored
firmware and the event log survive the deploy.
"""
import argparse
import sys

import paramiko

HOST = "192.168.1.99"
USER = "orangepi"
PASS = "OrangePiUser1!"
LOCAL = "ota_server.py"
LIVE = "/usr/local/bin/ota_server.py"
STAGE = "/tmp/ota_server.py.new"


def run(client, cmd, timeout=120):
    _, stdout, _ = client.exec_command(f"echo {PASS} | sudo -S {cmd}", timeout=timeout)
    out = stdout.read().decode(errors="replace")
    return out.replace("Password:", "").strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-restart", action="store_true")
    a = ap.parse_args()

    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=PASS, timeout=15,
              look_for_keys=False, allow_agent=False)
    print(f"[deploy] connected to {HOST}")

    sftp = c.open_sftp()
    sftp.put(LOCAL, STAGE)
    sftp.close()
    print(f"[deploy] uploaded {LOCAL} -> {STAGE}")

    # Compile before swapping: a syntax error would otherwise kill the service
    # and leave the fleet without an OTA endpoint.
    check = run(c, f"python3 -m py_compile {STAGE} && echo SYNTAX_OK 2>&1")
    print("[deploy] syntax check:", check or "(no output)")
    if "SYNTAX_OK" not in check:
        print("[deploy] ABORTED - the new file does not compile")
        c.close()
        sys.exit(1)

    print("[deploy] backup:", run(c, f"cp -a {LIVE} {LIVE}.bak && ls -l {LIVE}.bak"))
    print("[deploy] install:", run(c, f"install -m 0755 -o root -g root {STAGE} {LIVE}"))
    print("[deploy] version line:", run(c, f"sed -n '2p' {LIVE}"))

    if not a.no_restart:
        print("[deploy] restart:", run(c, "systemctl restart ota-firmware.service"))
        print("[deploy] status:", run(c, "systemctl is-active ota-firmware.service --no-pager"))
        print("[deploy] probe:", run(c, "curl -s -m 5 http://127.0.0.1:8080/ | head -3"))
        print("[deploy] log tail:", run(c, "curl -s -m 5 'http://127.0.0.1:8080/api/log?limit=2'"))

    c.close()


if __name__ == "__main__":
    main()
