#!/usr/bin/env python3
"""Run the provisioning script DETACHED on the board, then follow its log.

Why: streaming a long remote command over exec_command is fragile. If the client
dies, the remote stdout pipe breaks and the script is killed mid-way -- that is
exactly what happened the first time, leaving dpkg half-configured and steps 3-8
never executed.

This starts the script under `setsid nohup` with output redirected to a file on
the board, so it survives client disconnects, then polls that file.

Usage: python run_provision.py [ip] [--script path]
"""
from __future__ import annotations

import argparse
import os
import sys
import time

try:
    import paramiko
except ImportError:
    sys.exit("paramiko required")

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SCRIPT = os.path.join(HERE, "provision_rmq_baseline.sh")
REMOTE_SCRIPT = "/root/provision_rmq_baseline.sh"
REMOTE_LOG = "/root/provision.log"
DONE_MARKERS = ("DONE (baseline)", "=== PROVISIONED", "RESULT:")


def connect(ip, pw=os.environ.get("BOARD_ROOT_PW", "CHANGE_ME")):
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(ip, port=22, username="root", password=pw,
              look_for_keys=False, allow_agent=False, timeout=25,
              banner_timeout=30, auth_timeout=30)
    return c


def sh(c, cmd, t=120):
    _, so, se = c.exec_command(cmd, timeout=t)
    return so.read().decode("utf-8", "replace"), se.read().decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip", nargs="?", default="192.168.137.149")
    ap.add_argument("--script", default=DEFAULT_SCRIPT)
    ap.add_argument("--timeout", type=int, default=2400)
    args = ap.parse_args()

    c = connect(args.ip)
    try:
        print(f"== uploading {os.path.basename(args.script)} -> {REMOTE_SCRIPT}")
        sftp = c.open_sftp()
        try:
            sftp.put(args.script, REMOTE_SCRIPT)
        finally:
            sftp.close()

        out, err = sh(c, f"chmod +x {REMOTE_SCRIPT} && rm -f {REMOTE_LOG} && echo READY")
        print("  " + out.strip())

        start = (f"cd /root && setsid nohup bash {REMOTE_SCRIPT} "
                 f"> {REMOTE_LOG} 2>&1 < /dev/null & echo \"STARTED pid=$!\"")
        out, err = sh(c, start, t=30)
        print("  " + out.strip())
        if err.strip():
            print("  [stderr]", err.strip())

        print(f"\n== following {REMOTE_LOG} (script runs detached; safe if we disconnect)")
        seen = 0
        t0 = time.time()
        while True:
            out, _ = sh(c, f"cat {REMOTE_LOG} 2>/dev/null", t=60)
            if len(out) > seen:
                sys.stdout.write(out[seen:])
                sys.stdout.flush()
                seen = len(out)
            if any(m in out for m in DONE_MARKERS):
                print("\n== script reported completion")
                break
            running, _ = sh(c, "pgrep -f 'bash /root/provision_rmq_baseline.sh' | head -1")
            if not running.strip():
                print("\n== provision process is gone (check log above for why)")
                break
            if time.time() - t0 > args.timeout:
                print(f"\n== TIMEOUT after {args.timeout}s; script may still be running")
                break
            time.sleep(10)

        print("\n===== final state probe =====")
        for name, cmd in [
            ("rabbitmq service", "systemctl is-active rabbitmq-server; systemctl is-enabled rabbitmq-server"),
            ("version",          "timeout 90 rabbitmqctl version 2>&1 | tail -2"),
            ("vhosts",           "timeout 90 rabbitmqctl list_vhosts 2>&1 | tail -5"),
            ("users",            "timeout 90 rabbitmqctl list_users 2>&1 | tail -5"),
            ("listeners",        "ss -ltn 2>/dev/null | grep -E ':(1883|5672|15672|25672)' || echo NONE"),
            ("plugins",          "timeout 90 rabbitmq-plugins list -e -m 2>&1 | tail -6"),
            ("config",           "echo '--- rabbitmq.conf ---'; cat /etc/rabbitmq/rabbitmq.conf 2>/dev/null || echo MISSING; echo '--- env ---'; cat /etc/rabbitmq/rabbitmq-env.conf 2>/dev/null || echo MISSING"),
            ("mem",              "free -m | head -2"),
        ]:
            o, e = sh(c, cmd)
            print(f"--- {name}")
            if o.strip():
                print(o.rstrip())
            if e.strip():
                print("[stderr]", e.strip())
    finally:
        c.close()


if __name__ == "__main__":
    main()
