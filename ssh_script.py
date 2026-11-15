#!/usr/bin/env python3
"""Upload a local shell script to the Orange Pi and execute it with bash.

Usage:
    python ssh_script.py <local_script.sh> [sudo]

Prints stdout/stderr/exit code. `sudo` runs the remote script via
`echo <pass> | sudo -S bash <remote>`.
"""
import os
import sys

import paramiko

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HOST = "192.168.1.99"
USER = "orangepi"
PASS = "OrangePiUser1!"


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: ssh_script.py <local_script.sh> [sudo]")
    local = sys.argv[1]
    use_sudo = len(sys.argv) > 2 and sys.argv[2].lower() in ("sudo", "root")
    if not os.path.isfile(local):
        sys.exit(f"not found: {local}")

    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=PASS, timeout=20,
              look_for_keys=False, allow_agent=False)

    remote = "/tmp/" + os.path.basename(local)
    sftp = c.open_sftp()
    sftp.put(local, remote)
    sftp.chmod(remote, 0o755)
    sftp.close()

    if use_sudo:
        cmd = f"echo {PASS} | sudo -S bash {remote} 2>&1"
    else:
        cmd = f"bash {remote} 2>&1"

    stdin, stdout, stderr = c.exec_command(cmd, timeout=300)
    out = stdout.read().decode(errors="replace")
    err = stderr.read().decode(errors="replace")
    rc = stdout.channel.recv_exit_status()
    c.close()

    if out:
        print(out, end="" if out.endswith("\n") else "\n")
    if err:
        print("--- stderr ---")
        print(err, end="" if err.endswith("\n") else "\n")
    print(f"--- exit: {rc} ---")


if __name__ == "__main__":
    main()
