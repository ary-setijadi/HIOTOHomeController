#!/usr/bin/env python3
"""Bootstrap the freshly-flashed Armbian Orange Pi over SSH.

Goal: log in as root with the default password '1234', skip the interactive
first-run wizard, set a known root password, create a sudo user, and report
the board's identity/network state.

Usage:
    python3 ssh_bootstrap.py <ip> [new_root_pw] [username] [user_pw]
"""
import sys, time, re
import paramiko

IP = sys.argv[1] if len(sys.argv) > 1 else None
NEW_ROOT_PW = sys.argv[2] if len(sys.argv) > 2 else 'OrangePiRoot1!'
USER = sys.argv[3] if len(sys.argv) > 3 else 'orangepi'
USER_PW = sys.argv[4] if len(sys.argv) > 4 else 'OrangePiUser1!'

if not IP:
    print("usage: ssh_bootstrap.py <ip> [root_pw] [user] [user_pw]")
    sys.exit(2)

def run(c, cmd, timeout=60):
    print(f"\n--- EXEC: {cmd}")
    stdin, stdout, stderr = c.exec_command(cmd, timeout=timeout)
    out = stdout.read().decode('utf-8', 'replace')
    err = stderr.read().decode('utf-8', 'replace')
    print(out, end='')
    if err.strip():
        print("[stderr]", err, end='')
    return out, err

client = paramiko.SSHClient()
client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
print(f"== connecting to root@{IP} with password '1234'")
client.connect(IP, port=22, username='root', password='1234',
               look_for_keys=False, allow_agent=False, timeout=25)

# 1. Skip the interactive first-run wizard by removing its marker.
run(client, "rm -f /root/.not_logged_in_yet; echo SKIP_MARKER_DONE")

# 2. Set a known root password.
run(client, f"echo 'root:{NEW_ROOT_PW}' | chpasswd && echo ROOT_PW_SET")

# 3. Create a sudo user.
run(client, (
    f"id -u {USER} >/dev/null 2>&1 || useradd -m -s /bin/bash -G sudo {USER}; "
    f"echo '{USER}:{USER_PW}' | chpasswd && echo USER_SET"
))

# 4. Report identity + network.
run(client, "hostnamectl 2>/dev/null | head -6 || hostname")
run(client, "ip -4 addr show 2>/dev/null | grep -E 'inet ' ")
run(client, "ip route | head -5")
run(client, "systemctl is-enabled ssh sshd 2>/dev/null; systemctl is-active ssh sshd 2>/dev/null")

print("\n== DONE: root pw =", NEW_ROOT_PW, "user =", USER)
client.close()
