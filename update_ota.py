#!/usr/bin/env python3
"""Deploy the latest ota_server.py and restart the service."""
import sys, os, time, paramiko
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

ip, pw = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(ip, username='root', password=pw, look_for_keys=False, allow_agent=False, timeout=20)

sftp = c.open_sftp()
sftp.put(os.path.join(HERE, 'ota_server.py'), '/usr/local/bin/ota_server.py')
sftp.close()

i, o, e = c.exec_command('systemctl restart ota-firmware && sleep 2 && systemctl is-active ota-firmware && ss -tlnp | grep 8080', timeout=60)
print(o.read().decode('utf-8', 'replace'))
err = e.read().decode('utf-8', 'replace')
if err.strip():
    print('[stderr]', err)
c.close()
print('DEPLOYED')
