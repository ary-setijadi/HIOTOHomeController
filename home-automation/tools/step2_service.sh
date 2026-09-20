#!/bin/sh
sed -i 's|^ExecStart=.*|ExecStart=/root/controller-v4m -period-ms 1000 -db /var/lib/homeautomation/v4m.db -http-port 8081 -vhost /smarthome -user smarthome -password Ssm4rt2!|' /etc/systemd/system/controller-v4m.service
echo "--- updated ExecStart ---"
grep ExecStart /etc/systemd/system/controller-v4m.service
systemctl daemon-reload
systemctl restart controller-v4m
sleep 3
echo "--- status ---"
systemctl is-active controller-v4m
journalctl -u controller-v4m --no-pager -n 6
