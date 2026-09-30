#!/bin/bash
# failback.sh — return control to the primary (reverse of failover.sh).
# Run on the STAND-BY after the primary is back and has been re-synced.
set -euo pipefail

DNS_HOST="${DNS_HOST:-192.168.1.99}"
DNS_USER="${DNS_USER:-root}"
DNS_PASS="${DNS_PASS:-}"
PRIMARY_IP="${PRIMARY_IP:-192.168.1.22}"
RECORD="${RECORD:-MainController.hioto}"

if [ -n "$DNS_PASS" ]; then
  command -v sshpass >/dev/null 2>&1 || { echo "[failback] DNS_PASS set but sshpass missing" >&2; exit 1; }
  dns_ssh() { sshpass -p "$DNS_PASS" ssh -o StrictHostKeyChecking=accept-new "$DNS_USER@$DNS_HOST" "$@"; }
else
  dns_ssh() { ssh -o StrictHostKeyChecking=accept-new "$DNS_USER@$DNS_HOST" "$@"; }
fi

echo "[failback] pointing ${RECORD} -> ${PRIMARY_IP} on Pi-hole (${DNS_HOST})"
dns_ssh "pihole-FTL --config dns.hosts '[\"${PRIMARY_IP} ${RECORD}\"]'"

echo "[failback] stopping stand-by controller"
systemctl disable --now controller-v4m

echo "[failback] done: ${RECORD} -> ${PRIMARY_IP}; controller $(systemctl is-active controller-v4m)"
