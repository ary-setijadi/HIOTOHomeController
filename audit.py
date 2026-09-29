#!/usr/bin/env python3
import sys, paramiko
ip, pw = sys.argv[1], sys.argv[2]
c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(ip, username='root', password=pw, look_for_keys=False, allow_agent=False, timeout=20)
cmds = [
    "grep -E 'PermitRootLogin|PasswordAuthentication|^Port' /etc/ssh/sshd_config /etc/ssh/sshd_config.d/*.conf 2>/dev/null",
    "ss -tlnp",
    "ufw status 2>/dev/null || echo no-ufw",
    "nft list ruleset 2>/dev/null || echo no-nft",
    "pihole-FTL --config webserver.port",
    "pihole-FTL --config webserver.tls.cert",
]
for cmd in cmds:
    print('=== ' + cmd)
    i, o, e = c.exec_command(cmd, timeout=30)
    print(o.read().decode('utf-8', 'replace'))
    err = e.read().decode('utf-8', 'replace')
    if err.strip():
        print('[stderr]', err)
c.close()
