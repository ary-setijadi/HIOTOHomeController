#!/usr/bin/env python3
"""Dump Pi-hole domainlist (exact whitelist/blacklist/regex)."""
import sys, paramiko

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

IP = sys.argv[1] if len(sys.argv) > 1 else '192.168.1.99'
PW = sys.argv[2] if len(sys.argv) > 2 else 'OrangePiRoot1!'

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(IP, username='root', password=PW, look_for_keys=False,
          allow_agent=False, timeout=20)

code = (
    "import sqlite3\n"
    "db = sqlite3.connect('/etc/pihole/gravity.db')\n"
    "c = db.cursor()\n"
    "c.execute('SELECT type, domain, enabled, comment FROM domainlist ORDER BY type, domain')\n"
    "rows = c.fetchall()\n"
    "print('domainlist entries:', len(rows))\n"
    "for t, d, e, cm in rows:\n"
    "    cm = cm or ''\n"
    "    print(t.ljust(12), 'enabled=' + str(e), '|', d, '|', cm)\n"
)
sftp = c.open_sftp()
with sftp.open('/tmp/_dl.py', 'w') as f:
    f.write(code)
stdin, stdout, stderr = c.exec_command('python3 /tmp/_dl.py', timeout=60)
print(stdout.read().decode('utf-8', 'replace').strip())
err = stderr.read().decode('utf-8', 'replace')
if err.strip():
    print('[stderr]', err.strip())
sftp.remove('/tmp/_dl.py')
sftp.close()
c.close()
