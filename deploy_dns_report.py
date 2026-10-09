#!/usr/bin/env python3
"""Deploy dns_report.py + seed /etc/hioto/devices.json, then run the report."""
import sys, os, json, paramiko
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

ip, pw = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))

# Seed known devices (MAC -> name + category). Edit /etc/hioto/devices.json to refine.
seed = {
    "f8:75:a4:bf:d2:a0": {"name": "Management PC", "category": "trusted"},
    "02:42:7b:20:b4:b3": {"name": "Standby Controller (Docker)", "category": "controller"},
    "02:81:45:b3:5b:6b": {"name": "HiotoDNSServer", "category": "infrastructure"},
}

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(ip, username='root', password=pw, look_for_keys=False, allow_agent=False, timeout=20)

sftp = c.open_sftp()
sftp.put(os.path.join(HERE, 'dns_report.py'), '/usr/local/bin/dns_report.py')
sftp.close()

i, o, e = c.exec_command('mkdir -p /etc/hioto && cat > /etc/hioto/devices.json <<EOF\n%s\nEOF\nchmod +x /usr/local/bin/dns_report.py\n' % json.dumps(seed, indent=2), timeout=60)
o.read(); e.read()

i, o, e = c.exec_command('python3 /usr/local/bin/dns_report.py --text', timeout=60)
print(o.read().decode('utf-8', 'replace'))
err = e.read().decode('utf-8', 'replace')
if err.strip():
    print('[stderr]', err)
c.close()
