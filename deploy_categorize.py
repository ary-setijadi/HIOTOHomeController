#!/usr/bin/env python3
"""Categorize devices (routers), enable ECS, redeploy dashboard/report."""
import sys, os, io, json, paramiko
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

ip, pw = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))

devices = {
    "5c:02:14:09:8c:6d": {"name": "Xiaomi MiWiFi Router", "category": "router", "type": "router", "note": "NATs camera + phones (aggregate count)"},
    "4c:c6:4c:ae:09:b9": {"name": "Xiaomi MiWiFi Router", "category": "router", "type": "router", "note": "NATs phones (aggregate count)"},
    "3c:cd:57:a1:c8:08": {"name": "Xiaomi MiWiFi Router", "category": "router", "type": "router", "note": "aggregate count"},
    "28:ee:52:4e:e2:3d": {"name": "TP-Link Router", "category": "router", "type": "router", "note": "aggregate count"},
    "c4:71:54:cb:e4:3d": {"name": "TP-Link Router", "category": "router", "type": "router", "note": "aggregate count"},
    "02:42:7b:20:b4:b3": {"name": "Standby Controller", "category": "controller", "type": "host"},
    "f8:75:a4:bf:d2:a0": {"name": "Management PC", "category": "trusted", "type": "host"},
    "02:81:45:b3:5b:6b": {"name": "HiotoDNSServer", "category": "infrastructure", "type": "host"},
}

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
sftp.put(os.path.join(HERE, 'dns_report.py'), '/usr/local/bin/dns_report.py')
sftp.put(os.path.join(HERE, 'dashboard.html'), '/usr/local/bin/dashboard.html')
sftp.putfo(io.StringIO(json.dumps(devices, indent=2)), '/etc/hioto/devices.json')
sftp.close()

run('pihole-FTL --config dns.edns0ClientSubnet true')
run('pihole-FTL --config dns.edns0ClientSubnet')
run('systemctl restart monitor-server && sleep 2 && systemctl is-active monitor-server')
run('curl -s -o /dev/null -w "dashboard http=%{http_code} bytes=%{size_download}\\n" http://127.0.0.1:8081/')
run('curl -s http://127.0.0.1:8081/api/dns-clients | python3 -c "import sys,json; d=json.load(sys.stdin); [print(c[\"ip\"], c[\"category\"], c[\"type\"], c[\"name\"]) for c in d[\"clients\"]]"')
c.close()
print('CATEGORIZE + ECS DONE')
