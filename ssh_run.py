#!/usr/bin/env python3
"""Run a command on the Orange Pi over SSH and print its output.

Usage:
    python ssh_run.py "<command>"
"""
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
        sys.exit('usage: ssh_run.py "<command>"')
    cmd = sys.argv[1]

    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=PASS, timeout=20,
              look_for_keys=False, allow_agent=False)
    stdin, stdout, stderr = c.exec_command(cmd, timeout=180, get_pty=False)
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
