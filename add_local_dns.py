#!/usr/bin/env python3
"""Add a Pi-hole local DNS record: MainController.hioto -> 192.168.1.22"""
import sys, paramiko
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

run('pihole-FTL --config dns.hosts \'["192.168.1.22 MainController.hioto"]\'')
run('pihole-FTL --config dns.hosts')
run('dig MainController.hioto @127.0.0.1 +short')
c.close()
