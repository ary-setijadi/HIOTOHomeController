#!/usr/bin/env python3
"""Read Pi-hole v6 gravity.db blocklist stats."""
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
    "import sqlite3, os, datetime\n"
    "p = '/etc/pihole/gravity.db'\n"
    "print('gravity.db exists:', os.path.exists(p), 'size:', os.path.getsize(p) if os.path.exists(p) else 0)\n"
    "db = sqlite3.connect(p)\n"
    "c = db.cursor()\n"
    "c.execute(\"SELECT name FROM sqlite_master WHERE type='table' ORDER BY name\")\n"
    "print('tables:', [r[0] for r in c.fetchall()])\n"
    "c.execute('SELECT COUNT(*) FROM gravity')\n"
    "print('gravity domains (blocked):', c.fetchone()[0])\n"
    "c.execute('SELECT COUNT(*) FROM adlist')\n"
    "print('adlists:', c.fetchone()[0])\n"
    "c.execute(\"SELECT address FROM adlist LIMIT 5\")\n"
    "print('adlist sources:', [r[0] for r in c.fetchall()])\n"
    "c.execute('SELECT * FROM info')\n"
    "rows = c.fetchall()\n"
    "print('info:', rows)\n"
)
sftp = c.open_sftp()
with sftp.open('/tmp/_g.py', 'w') as f:
    f.write(code)
stdin, stdout, stderr = c.exec_command('python3 /tmp/_g.py', timeout=60)
print(stdout.read().decode('utf-8', 'replace').strip())
err = stderr.read().decode('utf-8', 'replace')
if err.strip():
    print('[stderr]', err.strip())
sftp.remove('/tmp/_g.py')
sftp.close()
c.close()
