#!/usr/bin/env python3
"""Bootstrap a freshly flashed Armbian Orange Pi Zero over SSH, then provision RabbitMQ.

Phase 1 (bootstrap) -- log in with Armbian's default root/1234, clear the
first-login marker so the interactive wizard never runs, set a known root
password, optionally create a sudo user, and report the board's identity.

Phase 2 (provision) -- upload a shell script and run it as root, streaming
output live.

Usage:
  python deploy_rmq.py <ip>
  python deploy_rmq.py <ip> --no-provision
  python deploy_rmq.py <ip> --script path/to/provision_rmq_baseline.sh
"""
from __future__ import annotations

import argparse
import os
import sys
import time

try:
    import paramiko
except ImportError:  # pragma: no cover
    sys.exit("paramiko is required:  python -m pip install paramiko")

DEFAULT_ROOT_PW = os.environ.get("BOARD_ROOT_PW", "CHANGE_ME")
DEFAULT_USER = "orangepi"
DEFAULT_USER_PW = os.environ.get("BOARD_USER_PW", "CHANGE_ME")
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SCRIPT = os.path.join(HERE, "provision_rmq_baseline.sh")


def connect(ip: str, password: str, user: str = "root", timeout: int = 25):
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(
        ip,
        port=22,
        username=user,
        password=password,
        look_for_keys=False,
        allow_agent=False,
        timeout=timeout,
        banner_timeout=30,
        auth_timeout=30,
    )
    return c


def open_session(ip: str, root_pw: str):
    """Try the target password first, then Armbian's default 1234."""
    errors = []
    for pw in (root_pw, "1234"):
        try:
            c = connect(ip, pw)
            print(f"== connected to root@{ip} (password {'<new>' if pw == root_pw else '1234'})")
            return c, pw
        except paramiko.AuthenticationException as e:
            errors.append(f"{pw}: auth failed")
        except Exception as e:  # noqa: BLE001
            errors.append(f"{pw}: {type(e).__name__}: {e}")
    raise SystemExit("could not authenticate to %s:\n  %s" % (ip, "\n  ".join(errors)))


def run(client, cmd: str, timeout: int = 300, stream: bool = True, label: str | None = None):
    if label:
        print(f"\n--- $ {label}")
    stdin, stdout, stderr = client.exec_command(cmd, timeout=timeout, get_pty=False)
    out_chunks: list[str] = []
    if stream:
        chan = stdout.channel
        while True:
            if chan.recv_ready():
                data = chan.recv(65536).decode("utf-8", "replace")
                out_chunks.append(data)
                print(data, end="", flush=True)
            if chan.exit_status_ready() and not chan.recv_ready():
                break
            time.sleep(0.05)
        while chan.recv_ready():
            data = chan.recv(65536).decode("utf-8", "replace")
            out_chunks.append(data)
            print(data, end="", flush=True)
    else:
        out_chunks.append(stdout.read().decode("utf-8", "replace"))
    err = stderr.read().decode("utf-8", "replace")
    rc = stdout.channel.recv_exit_status()
    if err.strip():
        print("[stderr]", err.rstrip())
    return "".join(out_chunks), err, rc


def bootstrap(client, root_pw: str, user: str, user_pw: str):
    print("\n===== PHASE 1: bootstrap =====")

    # Armbian gates the first login behind marker files; drop them so the
    # interactive wizard cannot hijack a non-interactive session.
    run(
        client,
        "rm -f /root/.not_logged_in_yet /root/.first_login /var/lib/armbian/.firstlogin 2>/dev/null; "
        "echo MARKERS_CLEARED",
        label="clear first-login markers",
    )
    run(
        client,
        f"echo 'root:{root_pw}' | chpasswd && echo ROOT_PW_SET",
        label="set root password",
    )
    run(
        client,
        f"id -u {user} >/dev/null 2>&1 || useradd -m -s /bin/bash -G sudo {user}; "
        f"echo '{user}:{user_pw}' | chpasswd && echo USER_SET",
        label=f"ensure sudo user {user}",
    )

    ident, _, _ = run(
        client,
        "echo '--- model ---'; "
        "cat /proc/device-tree/model 2>/dev/null | tr -d '\\0'; echo; "
        "echo '--- arch/host ---'; uname -m; hostname; "
        "echo '--- os ---'; . /etc/os-release && echo \"$PRETTY_NAME\"; "
        "echo '--- kernel ---'; uname -r; "
        "echo '--- mem ---'; free -m | head -2; "
        "echo '--- disk ---'; df -h / | tail -1; "
        "echo '--- ip ---'; ip -4 -o addr show scope global | awk '{print $2, $4}'; "
        "echo '--- ssh ---'; systemctl is-active ssh 2>/dev/null || systemctl is-active sshd",
        label="board identity",
    )
    return ident


def upload(client, local: str, remote: str):
    print(f"\n===== uploading {os.path.basename(local)} -> {remote}")
    sftp = client.open_sftp()
    try:
        sftp.put(local, remote)
    finally:
        sftp.close()
    print("upload complete")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip")
    ap.add_argument("--root-pw", default=DEFAULT_ROOT_PW)
    ap.add_argument("--user", default=DEFAULT_USER)
    ap.add_argument("--user-pw", default=DEFAULT_USER_PW)
    ap.add_argument("--script", default=DEFAULT_SCRIPT)
    ap.add_argument("--no-provision", action="store_true")
    ap.add_argument("--skip-bootstrap", action="store_true")
    args = ap.parse_args()

    client, used_pw = open_session(args.ip, args.root_pw)
    try:
        if not args.skip_bootstrap:
            bootstrap(client, args.root_pw, args.user, args.user_pw)

        if args.no_provision:
            print("\n--no-provision set: stopping after bootstrap.")
            return

        if not os.path.isfile(args.script):
            raise SystemExit(f"provisioning script not found: {args.script}")

        remote = "/root/" + os.path.basename(args.script)
        upload(client, args.script, remote)

        print("\n===== PHASE 2: provision RabbitMQ =====")
        out, err, rc = run(
            client,
            f"chmod +x {remote} && bash {remote}",
            timeout=3600,
            label=f"bash {remote}",
        )
        print(f"\n===== provision exit code: {rc}")
        if rc != 0:
            print("\n--- last 60 lines of syslog for rabbitmq ---")
            run(client, "journalctl -u rabbitmq-server --no-pager -n 60 2>/dev/null || tail -60 /var/log/rabbitmq/*.log 2>/dev/null", timeout=120)
            sys.exit(rc)
    finally:
        client.close()
    print("\nALL DONE")


if __name__ == "__main__":
    main()
