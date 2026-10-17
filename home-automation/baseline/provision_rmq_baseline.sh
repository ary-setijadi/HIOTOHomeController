#!/bin/bash
# Baseline RabbitMQ provisioning for a STANDALONE Orange Pi Zero (Armbian trixie).
#
# Scope: OS + RabbitMQ only. No TLS/stunnel, no controller, no coupling to the
# primary at 192.168.1.22 or the stand-by at 192.168.1.23 -- assign a role later.
#
# What it does:
#   1. sets a unique hostname (does NOT use 'maincontroller', which is taken)
#   2. installs rabbitmq-server (Debian trixie -> RabbitMQ 4.x + Erlang 27)
#   3. enables the rabbitmq_mqtt + rabbitmq_management plugins
#   4. applies the fleet's MQTT -> AMQP mapping (mqtt.exchange = home.automation)
#   5. tunes the Erlang VM + memory watermark for a 512 MB board
#   6. creates the /smarthome vhost and the fleet users/perms
#   7. starts the broker and verifies its listeners
#
# Usage (on the board, as root):  bash provision_rmq_baseline.sh [hostname]
set -euo pipefail

NEW_HOST="${1:-${HOSTNAME_NEW:-orangepizero}}"
VHOST=/smarthome
CERTDIR=/etc/rabbitmq/certs

log() { printf '\n=== %s\n' "$*"; }

if [ "$(id -u)" -ne 0 ]; then echo "must run as root" >&2; exit 1; fi

log "0. preflight"
echo "board arch : $(uname -m)"
echo "os         : $(. /etc/os-release && echo "$PRETTY_NAME")"
echo "kernel     : $(uname -r)"
echo "hostname   : $(hostname)  -> will become '${NEW_HOST}'"
free -m | sed -n '1,2p'
df -h / | sed -n '1,2p'

log "1. hostname -> ${NEW_HOST}"
hostnamectl set-hostname "$NEW_HOST" 2>/dev/null || true
grep -q "127.0.1.1 ${NEW_HOST}" /etc/hosts || echo "127.0.1.1 ${NEW_HOST}" >> /etc/hosts

log "2. install rabbitmq-server"
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y rabbitmq-server
# Stop whatever the postinst auto-started; we configure before the real start.
systemctl stop rabbitmq-server 2>/dev/null || true

log "3. broker config (fleet MQTT->AMQP mapping + 512 MB tuning)"
mkdir -p /etc/rabbitmq
if [ ! -f /etc/rabbitmq/rabbitmq.conf ]; then
  cat > /etc/rabbitmq/rabbitmq.conf <<'EOF'
# MQTT publishes map to AMQP routing keys on one shared topic exchange.
mqtt.exchange = home.automation

# 512 MB board: leave headroom for the OS / other services.
vm_memory_high_watermark.relative = 0.5

# Don't let a full SD card wedge the broker.
disk_free_limit.absolute = 300MB

# Classic queues are far cheaper than quorum queues on a 512 MB ARM board.
default_queue_type = classic
EOF
fi

# Limit Erlang schedulers/async threads: the default (one per core) and a large
# async pool cost memory we do not have on a 512 MB armv7 board.
cat > /etc/rabbitmq/rabbitmq-env.conf <<'EOF'
SERVER_ADDITIONAL_ERL_ARGS="+S 2:2 +A 4"
EOF

log "4. enable plugins"
rabbitmq-plugins enable rabbitmq_mqtt rabbitmq_management

log "5. Armbian: /var/log is zram (log2ram), so /var/log/rabbitmq is wiped on boot"
cat > /etc/tmpfiles.d/rabbitmq-server.conf <<'EOF'
d /var/log/rabbitmq 0750 rabbitmq rabbitmq -
d /var/lib/rabbitmq/mnesia 0750 rabbitmq rabbitmq -
EOF
systemd-tmpfiles --create /etc/tmpfiles.d/rabbitmq-server.conf 2>/dev/null || true

log "6. start broker + wait for readiness"
systemctl enable --now rabbitmq-server
ready=no
for i in $(seq 1 60); do
  if rabbitmqctl status >/dev/null 2>&1; then ready=yes; break; fi
  sleep 2
done
if [ "$ready" != yes ]; then
  echo "RabbitMQ did not become ready in 120s" >&2
  systemctl status rabbitmq-server --no-pager -l | tail -40 >&2 || true
  journalctl -u rabbitmq-server --no-pager -n 60 >&2 || true
  exit 1
fi
echo "broker ready after ${i} polls"

log "7. vhost + users + permissions"
# NOTE: replace 'CHANGE_ME' with the real fleet passwords before exposing the broker.
rabbitmqctl add_vhost "$VHOST" 2>/dev/null || true
rabbitmqctl add_user smarthome 'CHANGE_ME' 2>/dev/null || true
rabbitmqctl set_permissions -p "$VHOST" smarthome '.*' '.*' '.*'
rabbitmqctl add_user agent 'CHANGE_ME' 2>/dev/null || true
rabbitmqctl set_permissions -p "$VHOST" agent '.*' '.*' '.*'
rabbitmqctl set_topic_permissions -p "$VHOST" agent 'home.automation' '^Aktuator(\..*)?$' '.*' 2>/dev/null || true

log "8. verify"
echo "--- version ---"
rabbitmqctl version || true
echo "--- vhosts ---"
rabbitmqctl list_vhosts
echo "--- users ---"
rabbitmqctl list_users
echo "--- listeners (expect 1883 mqtt, 5672 amqp, 15672 mgmt) ---"
if command -v ss >/dev/null 2>&1; then ss -ltnp | grep -E ':(1883|5672|15672|25672)\b' || true; else netstat -ltnp | grep -E ':(1883|5672|15672|25672)\b' || true; fi
echo "--- enabled plugins ---"
rabbitmq-plugins list -e -m
echo "--- service state ---"
systemctl is-enabled rabbitmq-server; systemctl is-active rabbitmq-server

log "DONE (baseline)"
cat <<EOF

  hostname : ${NEW_HOST}
  vhost    : ${VHOST}
  users    : smarthome / agent
  MQTT     : 1883   (rabbitmq_mqtt, exchange home.automation)
  AMQP     : 5672
  mgmt UI  : http://${NEW_HOST}.local:15672  (or the board's IP, port 15672)

  NOT installed (deferred to role assignment): stunnel/MQTT-TLS 8883, AMQP-TLS
  5671, client certs, controller-v4m. Re-run home-automation/standby/provision.sh
  when you decide this board's role.

  Management UI default login is the local-only 'guest' user; create a real
  admin before exposing 15672:
    rabbitmqctl add_user admin 'CHANGE_ME'
    rabbitmqctl set_user_tags admin administrator
    rabbitmqctl set_permissions -p ${VHOST} admin '.*' '.*' '.*'
EOF
