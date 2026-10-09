#!/usr/bin/env python3
import sys, paramiko
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass
ip, pw = sys.argv[1], sys.argv[2]
c = paramiko.SSHClient(); c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(ip, username='root', password=pw, look_for_keys=False, allow_agent=False, timeout=20)
def run(cmd):
    i,o,e = c.exec_command(cmd, timeout=60)
    print('=== '+cmd)
    print(o.read().decode('utf-8','replace'))
    err = e.read().decode('utf-8','replace')
    if err.strip(): print('[stderr]', err)
run("pihole-FTL sqlite3 /etc/pihole/pihole-FTL.db 'SELECT id, timestamp, domain, client, status FROM queries ORDER BY id DESC LIMIT 5;'")
run("pihole-FTL sqlite3 /etc/pihole/pihole-FTL.db 'PRAGMA table_info(network);'")
run("pihole-FTL sqlite3 /etc/pihole/pihole-FTL.db 'PRAGMA table_info(network_addresses);'")
run("pihole-FTL sqlite3 /etc/pihole/pihole-FTL.db 'SELECT * FROM network_addresses LIMIT 10;'")
run("pihole-FTL sqlite3 /etc/pihole/pihole-FTL.db 'SELECT * FROM network LIMIT 10;'")
c.close()
