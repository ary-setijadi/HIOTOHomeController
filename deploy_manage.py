#!/usr/bin/env python3
"""Deploy the OTA management console (:8082) to the Orange Pi.

    python deploy_manage.py                    # install/upgrade, keep password
    python deploy_manage.py --reset-password   # install + issue a new password
    python deploy_manage.py --password 'secret'  # install with a chosen password

Copies ota_manage.py and ota-manage.service over SFTP, installs them with sudo,
creates /etc/ota-manage.json on first run, restarts the unit and smoke-tests
login plus the authenticated API.

Nothing here touches the device-facing OTA server (:8080) or the monitor
dashboard (:8081).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import secrets
import sys

import paramiko

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HOST = "192.168.1.99"
USER = "orangepi"
PASS = "OrangePiUser1!"
PORT = 8082
HERE = os.path.dirname(os.path.abspath(__file__))

UNIT = "ota-manage"
REMOTE_BIN = "/usr/local/bin/ota_manage.py"
REMOTE_UNIT = f"/etc/systemd/system/{UNIT}.service"
CONFIG = "/etc/ota-manage.json"


def hash_password(password: str, iterations: int = 200_000) -> str:
    salt = secrets.token_hex(16)
    dk = hashlib.pbkdf2_hmac("sha256", password.encode(), salt.encode(), iterations)
    return f"pbkdf2_sha256${iterations}${salt}${dk.hex()}"


class Pi:
    def __init__(self) -> None:
        self.c = paramiko.SSHClient()
        self.c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        self.c.connect(HOST, username=USER, password=PASS, timeout=20,
                       look_for_keys=False, allow_agent=False)
        self.sftp = self.c.open_sftp()

    def run(self, cmd: str, sudo: bool = False, timeout: int = 180) -> str:
        full = f"echo {PASS} | sudo -S bash -c {json.dumps(cmd)} 2>&1" if sudo else f"{cmd} 2>&1"
        _in, out, _err = self.c.exec_command(full, timeout=timeout)
        text = out.read().decode("utf-8", "replace")
        out.channel.recv_exit_status()
        return text

    def put(self, local: str, remote: str) -> None:
        self.sftp.put(local, remote)

    def close(self) -> None:
        try:
            self.sftp.close()
        finally:
            self.c.close()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--password", default="", help="set this console password")
    ap.add_argument("--reset-password", action="store_true",
                    help="generate and install a new console password")
    ap.add_argument("--keep-password", action="store_true",
                    help="never touch an existing /etc/ota-manage.json")
    a = ap.parse_args()

    for f in ("ota_manage.py", "ota-manage.service"):
        if not os.path.isfile(os.path.join(HERE, f)):
            sys.exit(f"missing local file: {f}")

    pi = Pi()
    print(f"[deploy] connected to {HOST}")

    # ---- upload to /tmp (world-writable staging) ----
    pi.put(os.path.join(HERE, "ota_manage.py"), "/tmp/ota_manage.py")
    pi.put(os.path.join(HERE, "ota-manage.service"), f"/tmp/{UNIT}.service")
    print("[deploy] uploaded ota_manage.py + unit to /tmp")

    # ---- config ----
    new_password = ""
    existing = pi.run(f"test -s {CONFIG} && echo YES || echo NO", sudo=True)
    have_config = "YES" in existing
    want_password = a.password or a.reset_password or (not have_config and not a.keep_password)

    if a.password:
        new_password = a.password
    elif a.reset_password or (not have_config and not a.keep_password):
        new_password = secrets.token_urlsafe(12)

    if want_password and new_password:
        cfg = {
            "user": "admin",
            "password_hash": hash_password(new_password),
            "secret": secrets.token_hex(32),
            "port": PORT,
            "created": "deploy",
        }
        local_cfg = os.path.join(HERE, ".ota-manage.json.local")
        with open(local_cfg, "w", encoding="utf-8") as fh:
            json.dump(cfg, fh, indent=2)
        pi.put(local_cfg, "/tmp/ota-manage.json")
        os.unlink(local_cfg)
        pi.run(f"install -m 600 -o root -g root /tmp/ota-manage.json {CONFIG}", sudo=True)
        print(f"[deploy] wrote {CONFIG}")
    else:
        print("[deploy] keeping the existing console credentials")

    # ---- install ----
    pi.run(f"install -m 755 -o root -g root /tmp/ota_manage.py {REMOTE_BIN}", sudo=True)
    pi.run(f"install -m 644 -o root -g root /tmp/{UNIT}.service {REMOTE_UNIT}", sudo=True)
    out = pi.run(
        f"systemctl daemon-reload && systemctl enable {UNIT} >/dev/null 2>&1; "
        f"systemctl restart {UNIT} && sleep 2 && systemctl is-active {UNIT}",
        sudo=True,
    )
    tail = [ln for ln in out.strip().splitlines() if ln.strip()]
    print(f"[deploy] service state: {tail[-1] if tail else 'unknown'}")

    # ---- smoke tests ----
    def http_code(url: str, extra: str = "") -> str:
        return pi.run(f"curl -s -o /dev/null -w '%{{http_code}}' {extra} {url}").strip()

    checks = []
    base = f"http://127.0.0.1:{PORT}"
    checks.append(("GET /login", http_code(base + "/login"), "200"))
    checks.append(("GET /api/overview (no auth)", http_code(base + "/api/overview"), "401"))
    checks.append(("GET / (no auth -> redirect)", http_code(base + "/"), "303"))

    if new_password:
        pi.run("rm -f /tmp/ck.txt")
        code = pi.run(
            f"curl -s -o /dev/null -w '%{{http_code}}' -c /tmp/ck.txt -X POST "
            f"-d 'user=admin&password={new_password}' {base}/login"
        ).strip()
        checks.append(("POST /login", code, "303"))
        body = pi.run(f"curl -s -b /tmp/ck.txt {base}/api/overview")
        ok = '"channels"' in body and '"fleet_counts"' in body
        checks.append(("GET /api/overview (auth)", "ok" if ok else "bad", "ok"))
        pi.run(f"curl -s -b /tmp/ck.txt {base}/api/fleet > /tmp/fleet.json")
        n = pi.run("python3 -c \"import json;d=json.load(open('/tmp/fleet.json'));"
                   "print(len(d['devices']))\"")
        checks.append(("GET /api/fleet devices", n.strip(), ">0"))
        pi.run("rm -f /tmp/ck.txt /tmp/fleet.json /tmp/ota_manage.py "
               "/tmp/ota-manage.json /tmp/ota-manage.service")

    print("\n[deploy] smoke tests")
    all_ok = True
    for name, got, want in checks:
        good = (got == want) or (want == ">0" and got.lstrip("-").isdigit() and int(got) > 0)
        all_ok = all_ok and good
        print(f"  {'PASS' if good else 'FAIL'}  {name:<34} got {got!r} want {want!r}")

    pi.close()

    print()
    if new_password:
        print("=" * 64)
        print("  OTA management console is live")
        print(f"    URL:      http://{HOST}:{PORT}/")
        print("    user:     admin")
        print(f"    password: {new_password}")
        print("  Change it under System -> Credentials.")
        print("=" * 64)
    else:
        print(f"  URL: http://{HOST}:{PORT}/  (existing password unchanged)")

    if not all_ok:
        sys.exit(1)


if __name__ == "__main__":
    main()
