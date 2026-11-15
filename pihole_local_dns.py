#!/usr/bin/env python3
"""Dump Pi-hole v6 local DNS configuration (records / domain / reverse / domainlist)."""
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


def run(cmd, timeout=60):
    stdin, stdout, stderr = c.exec_command(cmd, timeout=timeout)
    out = stdout.read().decode('utf-8', 'replace')
    err = stderr.read().decode('utf-8', 'replace')
    if err.strip():
        out += '[stderr] ' + err
    return out.strip()


print('=== LOCAL DNS RECORDS (dns.hosts = A/AAAA records) ===')
print(run('pihole-FTL --config dns.hosts'))
print()
print('=== LOCAL DOMAIN NAME (dns.domain) ===')
print(run('pihole-FTL --config dns.domain'))
print()
print('=== CONDITIONAL FORWARDING / REVERSE (dns.revServers) ===')
print(run('pihole-FTL --config dns.revServers'))
print()
print('=== CNAME RECORDS (dns.cnameRecords) ===')
print(run('pihole-FTL --config dns.cnameRecords'))
print()
print('=== INTERFACE / LISTEN BEHAVIOUR ===')
print(run('pihole-FTL --config dns.interface'))
print(run('pihole-FTL --config dns.listeningMode'))

# domainlist (whitelist/blacklist/regex) from gravity.db
code = (
    "import sqlite3\n"
    "db = sqlite3.connect('/etc/pihole/gravity.db')\n"
    "c = db.cursor()\n"
    "c.execute(\"SELECT type, domain, enabled, comment FROM domainlist ORDER BY type, domain\")\n"
    "rows = c.fetchall()\n"
    "print('=== DOMAINLIST (whitelist/blacklist/regex) entries:', len(rows), '===')\n"
    "for t, d, e, cm in rows:\n"
    "    print(f'{t:10} enabled={e}  {d}  {\"(cm or chr(34)+chr(34)) if cm else chr(34)+chr(34)}'.replace('  ',' | '))\n"
)
sftp = c.open_sftp()
with sftp.open('/tmp/_dl.py', 'w') as f:
    f.write(code)
print()
print(run('python3 /tmp/_dl.py'))
sftp.remove('/tmp/_dl.py')
sftp.close()
c.close()
