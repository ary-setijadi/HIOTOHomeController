#!/bin/bash
# Capture the controller's full state for the stand-by board.
# Run on the PRIMARY controller as root. Output: /root/standby-backup.tar.gz
# Usage: ./backup.sh [output.tar.gz]
set -euo pipefail

OUT="${1:-/root/standby-backup.tar.gz}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/var/lib/homeautomation" "$TMP/etc/rabbitmq" "$TMP/etc/stunnel" \
         "$TMP/etc/systemd/system" "$TMP/root"

# state (SQLite DB + telemetry rollover files)
cp -a /var/lib/homeautomation/. "$TMP/var/lib/homeautomation/" 2>/dev/null || true
# broker config + full certs dir (incl. ca.key + per-device certs)
cp -a /etc/rabbitmq/rabbitmq.conf "$TMP/etc/rabbitmq/" 2>/dev/null || true
cp -a /etc/rabbitmq/certs "$TMP/etc/rabbitmq/" 2>/dev/null || true
# stunnel config + certs
cp -a /etc/stunnel/mqtt-tls.conf "$TMP/etc/stunnel/" 2>/dev/null || true
cp -a /etc/stunnel/certs "$TMP/etc/stunnel/" 2>/dev/null || true
# controller binary + systemd unit
cp -a /root/controller-v4m "$TMP/root/" 2>/dev/null || true
cp -a /etc/systemd/system/controller-v4m.service "$TMP/etc/systemd/system/" 2>/dev/null || true

tar -czf "$OUT" -C "$TMP" .
echo "stand-by backup written to $OUT"
du -h "$OUT"
