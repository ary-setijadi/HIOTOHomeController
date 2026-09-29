#!/usr/bin/env python3
"""Change Pi static IP to 192.168.1.99/24 (gw/dns 192.168.1.1)."""
import sys, io, paramiko
ip, pw = sys.argv[1], sys.argv[2]

yaml = """network:
  version: 2
  renderer: networkd
  ethernets:
    end0:
      dhcp4: no
      dhcp6: no
      addresses:
        - 192.168.1.99/24
      routes:
        - to: default
          via: 192.168.1.1
      nameservers:
        addresses:
          - 192.168.1.1
"""

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(ip, username='root', password=pw, look_for_keys=False, allow_agent=False, timeout=20)

def run(cmd, timeout=60):
    i, o, e = c.exec_command(cmd, timeout=timeout)
    print('=== ' + cmd)
    try:
        print(o.read().decode('utf-8', 'replace'), end='')
    except Exception as ex:
        print('(read error)', ex)
    err = e.read().decode('utf-8', 'replace')
    if err.strip():
        print('[stderr]', err, end='')

sftp = c.open_sftp()
sftp.putfo(io.StringIO(yaml), '/etc/netplan/20-static-end0.yaml')
sftp.close()
run('chmod 600 /etc/netplan/20-static-end0.yaml && echo CHMODDED')
run('cat /etc/netplan/20-static-end0.yaml')
print('=== netplan apply (connection will drop) ===')
i, o, e = c.exec_command('netplan apply', timeout=30)
try:
    print(o.read().decode('utf-8', 'replace'), end='')
except Exception:
    print('(connection dropped as expected)')
c.close()
print('IP CHANGE TO 192.168.1.99 ISSUED')
