#!/usr/bin/env python3
"""Fix DNS: set Pi-hole upstreams and Pi resolver to the reachable gateway DNS."""
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
          - 192.168.31.1
          - 192.168.137.1
"""

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(ip, username='root', password=pw, look_for_keys=False, allow_agent=False, timeout=20)

sftp = c.open_sftp()
sftp.putfo(io.StringIO(yaml), '/etc/netplan/20-static-end0.yaml')
sftp.close()

def run(cmd):
    i, o, e = c.exec_command(cmd, timeout=90)
    print('=== ' + cmd)
    print(o.read().decode('utf-8', 'replace'), end='')
    err = e.read().decode('utf-8', 'replace')
    if err.strip():
        print('[stderr]', err, end='')

run('chmod 600 /etc/netplan/20-static-end0.yaml && echo CHMODDED')
run('netplan apply && echo APPLIED')
run('pihole-FTL --config dns.upstreams \'["192.168.31.1", "192.168.137.1"]\'')
run('pihole-FTL --config dns.upstreams')
time.sleep(5)
c.close()

# verify via FTL (127.0.0.1)
c2 = paramiko.SSHClient()
c2.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c2.connect(ip, username='root', password=pw, look_for_keys=False, allow_agent=False, timeout=20)
i, o, e = c2.exec_command('dig google.com A @127.0.0.1 +time=5 +tries=1 2>&1 | tail -5; echo ---AAAA---; dig google.com AAAA @127.0.0.1 +time=5 +tries=1 2>&1 | tail -4', timeout=60)
print(o.read().decode('utf-8', 'replace'))
c2.close()
print('DNS FIX DONE')
