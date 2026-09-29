#!/usr/bin/env python3
"""Set Pi to static 192.168.1.3/24 gw 192.168.1.1, update Pi-hole upstream, apply."""
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
        - 192.168.1.3/24
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

def run(cmd, timeout=90):
    i, o, e = c.exec_command(cmd, timeout=timeout)
    print('=== ' + cmd)
    print(o.read().decode('utf-8', 'replace'), end='')
    err = e.read().decode('utf-8', 'replace')
    if err.strip():
        print('[stderr]', err, end='')

# 1. Update Pi-hole upstream to the new gateway DNS (while still connected)
run('pihole-FTL --config dns.upstreams \'["192.168.1.1"]\'')
run('pihole-FTL --config dns.upstreams')

# 2. Write new netplan config
sftp = c.open_sftp()
sftp.putfo(io.StringIO(yaml), '/etc/netplan/20-static-end0.yaml')
sftp.close()
run('chmod 600 /etc/netplan/20-static-end0.yaml && echo CHMODDED')
run('cat /etc/netplan/20-static-end0.yaml')

# 3. Apply (this will change the IP and drop this SSH session)
print('=== netplan apply (connection will drop) ===')
i, o, e = c.exec_command('netplan apply && echo APPLIED', timeout=30)
try:
    print(o.read().decode('utf-8', 'replace'), end='')
except Exception as ex:
    print('(connection dropped as expected)', ex)
c.close()
print('IP CHANGE ISSUED')
