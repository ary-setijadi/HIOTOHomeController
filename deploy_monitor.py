#!/usr/bin/env python3
"""Deploy the monitor server + service, start it, and smoke-test."""
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
sftp.put(os.path.join(HERE, 'monitor-server.service'), '/etc/systemd/system/monitor-server.service')
sftp.close()

run('chmod +x /usr/local/bin/monitor_server.py')
run('systemctl daemon-reload && systemctl enable monitor-server && systemctl restart monitor-server && sleep 2 && systemctl is-active monitor-server')
run('curl -s http://127.0.0.1:8081/ ; echo')
run('curl -s http://127.0.0.1:8081/api/dns-clients | python3 -c "import sys,json; d=json.load(sys.stdin); print(json.dumps(d[\"clients\"][:3], indent=2))"')
c.close()
print('MONITOR DEPLOYED')
