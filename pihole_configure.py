#!/usr/bin/env python3
"""Configure Pi-hole v6 on the Pi: upstream DNS + web password, then verify."""
import sys, paramiko
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass
ip = sys.argv[1]
pw = sys.argv[2]
webpw = 'OrangePiWeb2026'

def run(c, cmd):
    print('=== ' + cmd)
    stdin, stdout, stderr = c.exec_command(cmd, timeout=120)
    out = stdout.read().decode('utf-8', 'replace')
    err = stderr.read().decode('utf-8', 'replace')
    print(out, end='')
    if err.strip():
        print('[stderr]', err, end='')

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(ip, username='root', password=pw, look_for_keys=False, allow_agent=False, timeout=20)

# 1. Set upstream DNS servers (Cloudflare)
run(c, 'pihole-FTL --config dns.upstreams \'["1.1.1.1", "1.0.0.1"]\'')
# 2. Set web admin password
run(c, 'pihole setpassword ' + webpw + ' < /dev/null')
# 3. Verify upstreams
run(c, 'pihole-FTL --config dns.upstreams')
# 4. Status
run(c, 'pihole status')
c.close()
print('\nWEB PASSWORD set to: ' + webpw)
