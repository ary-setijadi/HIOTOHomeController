#!/bin/bash
# Provision this board as a STAND-BY controller (identical to the primary).
# Run as root on a clean Armbian board (same architecture as the primary).
# Usage: ./provision.sh [/path/to/standby-backup.tar.gz]
set -euo pipefail

BACKUP="${1:-}"

echo "=== 0. reset to a clean state ==="
systemctl stop controller-v4m 2>/dev/null || true
systemctl stop stunnel4 2>/dev/null || true
systemctl stop rabbitmq-server 2>/dev/null || true
rm -rf /var/lib/rabbitmq /var/lib/homeautomation /etc/rabbitmq /etc/stunnel

echo "=== 1. hostname (match primary so Erlang node + certs line up) ==="
hostnamectl set-hostname maincontroller 2>/dev/null || true
grep -q '127.0.0.1 maincontroller' /etc/hosts || echo '127.0.0.1 maincontroller' >> /etc/hosts

echo "=== 2. install packages ==="
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y rabbitmq-server stunnel4
systemctl stop rabbitmq-server stunnel4 2>/dev/null || true   # stop auto-started instances

echo "=== 3. restore backup (config + certs + state + binary) ==="
if [ -n "$BACKUP" ] && [ -f "$BACKUP" ]; then
  tar -xzf "$BACKUP" -C /
  chown -R rabbitmq:rabbitmq /etc/rabbitmq/certs 2>/dev/null || true
  chmod 600 /etc/rabbitmq/certs/*.key 2>/dev/null || true
  chmod 644 /etc/rabbitmq/certs/*.crt 2>/dev/null || true
  chown -R stunnel4:stunnel4 /etc/stunnel/certs 2>/dev/null || true
  chmod 600 /etc/stunnel/certs/server.key 2>/dev/null || true
  chmod 644 /etc/stunnel/certs/*.crt 2>/dev/null || true
  chmod +x /root/controller-v4m 2>/dev/null || true
else
  echo "WARN: no backup tarball given — certs and state will be missing."
fi

echo "=== 4. broker config + plugins ==="
if [ ! -f /etc/rabbitmq/rabbitmq.conf ]; then
  cat > /etc/rabbitmq/rabbitmq.conf <<'EOF'
mqtt.exchange = home.automation
listeners.ssl.default = 5671
ssl_options.cacertfile = /etc/rabbitmq/certs/ca.crt
ssl_options.certfile = /etc/rabbitmq/certs/rabbitmq-server.crt
ssl_options.keyfile = /etc/rabbitmq/certs/rabbitmq-server.key
ssl_options.verify = verify_none
ssl_options.fail_if_no_peer_cert = false
EOF
fi
rabbitmq-plugins enable rabbitmq_mqtt rabbitmq_management 2>/dev/null || true

echo "=== 5. start RabbitMQ + vhost/users/perms ==="
systemctl enable --now rabbitmq-server
sleep 5
rabbitmqctl add_vhost /smarthome 2>/dev/null || true
rabbitmqctl add_user smarthome 'Ssm4rt2!' 2>/dev/null || true
rabbitmqctl set_permissions -p /smarthome smarthome '.*' '.*' '.*' 2>/dev/null || true
rabbitmqctl add_user agent 'Agent!23' 2>/dev/null || true
rabbitmqctl set_permissions -p /smarthome agent '.*' '.*' '.*' 2>/dev/null || true
rabbitmqctl set_topic_permissions -p /smarthome agent 'home.automation' '^Aktuator(\..*)?$' '.*' 2>/dev/null || true

echo "=== 6. stunnel (MQTT TLS terminator 8883) ==="
if [ ! -f /etc/stunnel/mqtt-tls.conf ]; then
  cat > /etc/stunnel/mqtt-tls.conf <<'EOF'
[mqtt-tls]
client  = no
accept  = 8883
connect = 127.0.0.1:1883
cert    = /etc/stunnel/certs/server.crt
key     = /etc/stunnel/certs/server.key
CAfile  = /etc/stunnel/certs/ca.crt
verify  = 2
EOF
fi
if [ ! -f /etc/stunnel/certs/server.crt ] && [ -f /etc/rabbitmq/certs/rabbitmq-server.crt ]; then
  mkdir -p /etc/stunnel/certs
  cp /etc/rabbitmq/certs/rabbitmq-server.crt /etc/stunnel/certs/server.crt
  cp /etc/rabbitmq/certs/rabbitmq-server.key /etc/stunnel/certs/server.key
  cp /etc/rabbitmq/certs/ca.crt /etc/stunnel/certs/ca.crt
  chown -R stunnel4:stunnel4 /etc/stunnel/certs
  chmod 600 /etc/stunnel/certs/server.key
  chmod 644 /etc/stunnel/certs/*.crt
fi
systemctl enable --now stunnel4

echo "=== 7. controller (installed, DISABLED until failover) ==="
if [ ! -f /etc/systemd/system/controller-v4m.service ]; then
  cat > /etc/systemd/system/controller-v4m.service <<'EOF'
[Unit]
After=rabbitmq-server.service
Wants=rabbitmq-server.service

[Service]
Type=simple
ExecStart=/root/controller-v4m -period-ms 1000 -db /var/lib/homeautomation/v4m.db \
  -http-port 8081 -broker-host 192.168.1.22 -vhost /smarthome \
  -user smarthome -password 'Ssm4rt2!'
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
fi
systemctl daemon-reload
systemctl disable controller-v4m 2>/dev/null || true
systemctl stop controller-v4m 2>/dev/null || true

echo
echo "=== PROVISIONED (stand-by) ==="
echo "rabbitmq:   $(systemctl is-active rabbitmq-server)"
echo "stunnel:    $(systemctl is-active stunnel4)"
echo "controller: $(systemctl is-enabled controller-v4m 2>/dev/null || echo disabled) (start on failover)"
echo
echo "On failover: take IP 192.168.1.22, then:  systemctl enable --now controller-v4m"
