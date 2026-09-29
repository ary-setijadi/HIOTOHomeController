#!/usr/bin/env python3
"""Set a static IP on end0 via netplan and apply it."""
import sys, io, time, paramiko
ip, pw = sys.argv[1], sys.argv[2]

yaml = """network:
  version: 2
  renderer: networkd
  ethernets:
    end0:
      dhcp4: no
      dhcp6: no
      addresses:
        - 192.168.137.165/24
      routes:
        - to: default
          via: 192.168.137.1
      nameservers:
        addresses:
          - 1.1.1.1
          - 1.0.0.1
"""

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(ip, username='root', password=pw, look_for_keys=False, allow_agent=False, timeout=20)

# Write new netplan file via SFTP
sftp = c.open_sftp()
sftp.putfo(io.StringIO(yaml), '/etc/netplan/20-static-end0.yaml')
sftp.close()

def run(cmd):
    i, o, e = c.exec_command(cmd, timeout=90)
    out = o.read().decode('utf-8', 'replace')
    err = e.read().decode('utf-8', 'replace')
    print('=== ' + cmd)
    print(out, end='')
    if err.strip():
        print('[stderr]', err, end='')

run('mv -f /etc/netplan/10-dhcp-all-interfaces.yaml /etc/netplan/10-dhcp-all-interfaces.yaml.bak && echo RENAMED')
run('netplan apply && echo APPLIED')
print('--- waiting 8s for network settle ---')
time.sleep(8)
c.close()

# Reconnect and verify
c2 = paramiko.SSHClient()
c2.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c2.connect(ip, username='root', password=pw, look_for_keys=False, allow_agent=False, timeout=20)
i, o, e = c2.exec_command('ip -4 addr show end0; echo ---; ip route', timeout=30)
print(o.read().decode('utf-8', 'replace'))
c2.close()
print('STATIC IP DONE')
