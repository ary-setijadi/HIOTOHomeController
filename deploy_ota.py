#!/usr/bin/env python3
"""Deploy the OTA server v2 (device-type aware + logging) and smoke-test it."""
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
    i, o, e = c.exec_command(cmd, timeout=120)
    print('=== ' + cmd)
    print(o.read().decode('utf-8', 'replace'), end='')
    err = e.read().decode('utf-8', 'replace')
    if err.strip():
        print('[stderr]', err, end='')

sftp = c.open_sftp()
sftp.put(os.path.join(HERE, 'ota_server.py'), '/usr/local/bin/ota_server.py')
sftp.put(os.path.join(HERE, 'publish_firmware.py'), '/usr/local/bin/publish_firmware.py')
sftp.put(os.path.join(HERE, 'ota-firmware.service'), '/etc/systemd/system/ota-firmware.service')
sftp.close()

run('chmod +x /usr/local/bin/ota_server.py /usr/local/bin/publish_firmware.py')
run('systemctl daemon-reload && systemctl restart ota-firmware && systemctl is-active ota-firmware')

# publish placeholder firmware for two device types
run("printf 'esp32 firmware 1.0.0\\n' > /tmp/fw-esp32-1.0.0.bin")
run('python3 /usr/local/bin/publish_firmware.py /tmp/fw-esp32-1.0.0.bin --device esp32 --version 1.0.0 --note "esp32 test build"')
run("printf 'controller firmware 2.3.1\\n' > /tmp/fw-controller-2.3.1.bin")
run('python3 /usr/local/bin/publish_firmware.py /tmp/fw-controller-2.3.1.bin --device controller --version 2.3.1 --note "controller test build"')

# smoke tests
run('curl -s http://127.0.0.1:8080/api/devices; echo')
run('curl -s "http://127.0.0.1:8080/api/latest?device=esp32"; echo')
run('curl -s -o /dev/null -w "esp32 firmware download http=%{http_code} size=%{size_download}\\n" http://127.0.0.1:8080/firmware/esp32/fw-esp32-1.0.0.bin')
run('curl -s "http://127.0.0.1:8080/api/log?limit=20"; echo')

c.close()
print('OTA V2 DEPLOY DONE')
