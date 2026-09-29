#!/usr/bin/env python3
"""Rename the Pi's hostname to HiotoDNSServer."""
import sys, paramiko
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

ip, pw = sys.argv[1], sys.argv[2]
c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(ip, username='root', password=pw, look_for_keys=False, allow_agent=False, timeout=20)

def run(cmd):
    i, o, e = c.exec_command(cmd, timeout=60)
    print('=== ' + cmd)
    print(o.read().decode('utf-8', 'replace'), end='')
    err = e.read().decode('utf-8', 'replace')
    if err.strip():
        print('[stderr]', err, end='')

run("hostnamectl set-hostname HiotoDNSServer")
run("sed -i 's/orangepipc/HiotoDNSServer/g' /etc/hosts")
run("hostnamectl")
run("cat /etc/hosts")
run("hostname")
c.close()
print('HOSTNAME SET')
