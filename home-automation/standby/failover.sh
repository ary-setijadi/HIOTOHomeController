#!/bin/bash
# failover.sh — promote this stand-by to primary via a DNS swap on the Pi-hole.
# Run on the STAND-BY (192.168.1.23) when the primary is down.
#
# Requires SSH access to the Pi-hole (HiotoDNSServer, 192.168.1.99):
#   - key auth recommended, or set DNS_PASS (requires `sshpass` here)
set -euo pipefail

DNS_HOST="${DNS_HOST:-192.168.1.99}"
DNS_USER="${DNS_USER:-root}"
DNS_PASS="${DNS_PASS:-}"
SELF_IP="${SELF_IP:-192.168.1.23}"
RECORD="${RECORD:-MainController.hioto}"

if [ -n "$DNS_PASS" ]; then
  command -v sshpass >/dev/null 2>&1 || { echo "[failover] DNS_PASS set but sshpass missing" >&2; exit 1; }
  dns_ssh() { sshpass -p "$DNS_PASS" ssh -o StrictHostKeyChecking=accept-new "$DNS_USER@$DNS_HOST" "$@"; }
else
  dns_ssh() { ssh -o StrictHostKeyChecking=accept-new "$DNS_USER@$DNS_HOST" "$@"; }
fi

echo "[failover] pointing ${RECORD} -> ${SELF_IP} on Pi-hole (${DNS_HOST})"
dns_ssh "pihole-FTL --config dns.hosts '[\"${SELF_IP} ${RECORD}\"]'"

echo "[failover] starting controller"
systemctl enable --now controller-v4m

echo "[failover] done: ${RECORD} -> ${SELF_IP}; controller $(systemctl is-active controller-v4m)"
