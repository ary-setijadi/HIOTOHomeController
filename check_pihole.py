#!/usr/bin/env python3
"""Query-log stats + gravity count + HTTP health, on-device."""
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


def run(cmd, timeout=120):
    stdin, stdout, stderr = c.exec_command(cmd, timeout=timeout)
    out = stdout.read().decode('utf-8', 'replace')
    err = stderr.read().decode('utf-8', 'replace')
    if err.strip():
        out += '\n[stderr] ' + err
    return out.strip()


stats = (
    "import sqlite3, datetime\n"
    "db = sqlite3.connect('/etc/pihole/pihole-FTL.db')\n"
    "c = db.cursor()\n"
    "c.execute('SELECT COUNT(*) FROM queries')\n"
    "print('total queries   :', c.fetchone()[0])\n"
    "c.execute(\"SELECT COUNT(*) FROM queries WHERE status IN (1,4,5,6,7,8,9,10,11)\")\n"
    "print('blocked queries :', c.fetchone()[0])\n"
    "c.execute(\"SELECT COUNT(*) FROM queries WHERE status IN (2,3)\")\n"
    "print('forwarded       :', c.fetchone()[0])\n"
    "c.execute('SELECT COUNT(DISTINCT client) FROM queries')\n"
    "print('distinct clients:', c.fetchone()[0])\n"
    "c.execute('SELECT timestamp FROM queries ORDER BY id DESC LIMIT 1')\n"
    "ts = c.fetchone()[0]\n"
    "print('last query epoch:', ts)\n"
    "if ts: print('last query time :', datetime.datetime.fromtimestamp(ts).strftime('%Y-%m-%d %H:%M:%S'))\n"
    "c.execute(\"SELECT name FROM sqlite_master WHERE type='table' ORDER BY name\")\n"
    "print('tables:', [r[0] for r in c.fetchall()])\n"
    "for t in ['gravity','domainlist','adlist']:\n"
    "    try:\n"
    "        c.execute('SELECT COUNT(*) FROM '+t)\n"
    "        print(t, 'rows:', c.fetchone()[0])\n"
    "    except Exception as e:\n"
    "        print(t, 'ERR', e)\n"
)
sftp = c.open_sftp()
with sftp.open('/tmp/_stats.py', 'w') as f:
    f.write(stats)
print('=== QUERY LOG + GRAVITY ===')
print(run('python3 /tmp/_stats.py'))
sftp.remove('/tmp/_stats.py')
sftp.close()

print()
print('=== HTTP HEALTH (local curl) ===')
print(run("curl -s -o /dev/null -w 'web-admin(80): %{http_code}\n' http://127.0.0.1/admin/ 2>/dev/null || true"))
print(run("curl -s -o /dev/null -w 'dashboard(8081): %{http_code}\n' http://127.0.0.1:8081/ 2>/dev/null || true"))
print(run("curl -s -o /dev/null -w 'ota(8080): %{http_code}\n' http://127.0.0.1:8080/api/devices 2>/dev/null || true"))
print()
print('=== DASHBOARD API SUMMARY ===')
print(run("curl -s http://127.0.0.1:8081/api/dns-clients 2>/dev/null | head -c 800 || true"))

c.close()
