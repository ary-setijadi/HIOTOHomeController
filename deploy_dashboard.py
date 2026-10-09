#!/usr/bin/env python3
"""Deploy the dashboard + updated monitor server, restart, and verify."""
import sys, os, paramiko
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

def run(cmd):
    i, o, e = c.exec_command(cmd, timeout=90)
    print('=== ' + cmd)
    print(o.read().decode('utf-8', 'replace'), end='')
    err = e.read().decode('utf-8', 'replace')
    if err.strip():
        print('[stderr]', err, end='')

sftp = c.open_sftp()
sftp.put(os.path.join(HERE, 'monitor_server.py'), '/usr/local/bin/monitor_server.py')
sftp.put(os.path.join(HERE, 'dashboard.html'), '/usr/local/bin/dashboard.html')
sftp.close()

run('systemctl restart monitor-server && sleep 2 && systemctl is-active monitor-server')
run('curl -s -o /dev/null -w "dashboard http=%{http_code} bytes=%{size_download}\\n" http://127.0.0.1:8081/')
c.close()
print('DASHBOARD DEPLOYED')
