#!/usr/bin/env python3
"""Run one command on the Pi over SSH (paramiko) and print output.
Usage: python3 ssh_cmd.py <ip> <password> <command>
"""
import sys, paramiko
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass
ip, pw, cmd = sys.argv[1], sys.argv[2], sys.argv[3]
c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(ip, username='root', password=pw, look_for_keys=False, allow_agent=False, timeout=20)
stdin, stdout, stderr = c.exec_command(cmd, timeout=300)
out = stdout.read().decode('utf-8', 'replace')
err = stderr.read().decode('utf-8', 'replace')
print(out, end='')
if err.strip():
    print('[stderr]', err, end='')
c.close()
