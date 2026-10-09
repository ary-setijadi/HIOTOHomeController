#!/usr/bin/env python3
import sys, json, urllib.request, paramiko
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass
ip, pw = sys.argv[1], sys.argv[2]

c = paramiko.SSHClient(); c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(ip, username='root', password=pw, look_for_keys=False, allow_agent=False, timeout=20)
i, o, e = c.exec_command("pihole-FTL --config dns.EDNS0ECS; echo ---; cat /etc/hioto/devices.json", timeout=30)
print(o.read().decode('utf-8', 'replace'))
c.close()

# query the HTTP API from the PC side (clean, no shell quoting)
try:
    with urllib.request.urlopen('http://%s:8081/api/dns-clients' % ip, timeout=15) as r:
        d = json.load(r)
    print('\n=== clients (ip | category | type | name) ===')
    for cl in d['clients']:
        print('%-16s %-15s %-6s %s' % (cl['ip'], cl['category'], cl['type'], cl['name']))
except Exception as ex:
    print('API error:', ex)
