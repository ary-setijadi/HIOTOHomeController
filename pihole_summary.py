#!/usr/bin/env python3
"""Fetch Pi-hole API summary (gravity + query totals)."""
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
    "import json,urllib.request\n"
    "d = json.load(urllib.request.urlopen('http://127.0.0.1/api/stats/summary'))\n"
    "g = d.get('gravity', {})\n"
    "q = d.get('queries', {})\n"
    "print('domains_being_blocked :', g.get('domains_being_blocked'))\n"
    "print('last_update           :', g.get('last_update'))\n"
    "print('dns_queries_total     :', q.get('total'))\n"
    "print('queries_blocked       :', q.get('blocked'))\n"
    "print('percent_blocked       :', q.get('percent_blocked'))\n"
    "print('clients_seen          :', q.get('clients'))\n"
    "print('queries_forwarded     :', q.get('forwarded'))\n"
    "print('queries_cached        :', q.get('cached'))\n"
)
sftp = c.open_sftp()
with sftp.open('/tmp/_sum.py', 'w') as f:
    f.write(code)
stdin, stdout, stderr = c.exec_command('python3 /tmp/_sum.py', timeout=60)
print(stdout.read().decode('utf-8', 'replace').strip())
err = stderr.read().decode('utf-8', 'replace')
if err.strip():
    print('[stderr]', err.strip())
sftp.remove('/tmp/_sum.py')
sftp.close()
c.close()
