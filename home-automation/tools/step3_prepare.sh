#!/bin/sh
echo "=== SAVE static IP 192.168.1.22 (applies on next boot) ==="
nmcli con mod "Wired connection 1" ipv4.method manual ipv4.addresses 192.168.1.22/24 ipv4.gateway 192.168.1.1 ipv4.dns "8.8.8.8 1.1.1.1"
nmcli con show "Wired connection 1" | grep -E "ipv4.method|ipv4.addresses|ipv4.gateway|ipv4.dns"

echo "=== update controller service (broker-host for QR) ==="
sed -i 's|-http-port 8081|-http-port 8081 -broker-host 192.168.1.22|' /etc/systemd/system/controller-v4m.service
grep ExecStart /etc/systemd/system/controller-v4m.service
systemctl daemon-reload

echo "=== services status ==="
systemctl is-active controller-v4m rabbitmq-server
echo "=== current IP (still reachable now) ==="
ip -4 addr show eth0 | grep inet
