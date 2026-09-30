#!/bin/bash
# sync-from-primary.sh — one-way PULL of the primary controller's mutable state
# onto this stand-by (192.168.1.23). The stand-by never runs the controller
# during sync and only talks to the primary during these scheduled pulls.
#
# Run on the STAND-BY, as root, on your chosen schedule (see
# sync-from-primary.service / .timer). RPO = the schedule interval.
#
# SSH access to the primary is required:
#   - recommended: key auth   ssh-copy-id root@MainController.hioto
#   - or set PRIMARY_PASS (requires `sshpass` installed here: apt-get install sshpass)
set -euo pipefail

PRIMARY_HOST="${PRIMARY_HOST:-MainController.hioto}"
PRIMARY_USER="${PRIMARY_USER:-root}"
PRIMARY_PASS="${PRIMARY_PASS:-}"        # optional; forces sshpass when set
SYNC_BINARY="${SYNC_BINARY:-1}"         # also pull a newer controller binary

if [ -n "$PRIMARY_PASS" ]; then
  command -v sshpass >/dev/null 2>&1 || { echo "[sync] PRIMARY_PASS set but sshpass missing" >&2; exit 1; }
  RSYNC_RSH="sshpass -p '$PRIMARY_PASS' ssh"
  ssh_primary() { sshpass -p "$PRIMARY_PASS" ssh -o StrictHostKeyChecking=accept-new "$PRIMARY_USER@$PRIMARY_HOST" "$@"; }
else
  RSYNC_RSH="ssh"
  ssh_primary() { ssh -o StrictHostKeyChecking=accept-new "$PRIMARY_USER@$PRIMARY_HOST" "$@"; }
fi
SRC="${PRIMARY_USER}@${PRIMARY_HOST}"

# stable one-line fingerprint of a directory's file contents
fingerprint() {
  ( cd "$1" 2>/dev/null && find . -type f -print0 | sort -z | xargs -0 -r sha256sum 2>/dev/null ) | sha256sum
}

# skip if a previous run is still going
exec 9>/run/sync-from-primary.lock
flock -n 9 || { echo "[sync] already running; skipping" >&2; exit 0; }

echo "[sync] $(date -Is) pulling ${SRC}"

# 0. The stand-by must never run the controller during normal operation.
systemctl stop controller-v4m 2>/dev/null || true

# 1. Best-effort: consolidate the primary's WAL for a consistent DB copy.
ssh_primary "command -v sqlite3 >/dev/null 2>&1 && sqlite3 /var/lib/homeautomation/v4m.db 'PRAGMA wal_checkpoint(TRUNCATE);' || true" 2>/dev/null || true

# 2. Mutable data state: device registry + rules + timers + telemetry.
mkdir -p /var/lib/homeautomation
rsync -a --delete -e "$RSYNC_RSH" "${SRC}:/var/lib/homeautomation/" /var/lib/homeautomation/

# 3. Identity: CA + server certs (incl. ca.key) so devices/agent verify the
#    stand-by exactly like the primary. stunnel certs are derived copies.
before=$(fingerprint /etc/rabbitmq/certs)
mkdir -p /etc/rabbitmq/certs /etc/stunnel/certs
rsync -a --delete --checksum -e "$RSYNC_RSH" "${SRC}:/etc/rabbitmq/certs/" /etc/rabbitmq/certs/
chown -R rabbitmq:rabbitmq /etc/rabbitmq/certs
chmod 600 /etc/rabbitmq/certs/*.key 2>/dev/null || true
chmod 644 /etc/rabbitmq/certs/*.crt 2>/dev/null || true
cp /etc/rabbitmq/certs/rabbitmq-server.crt /etc/stunnel/certs/server.crt 2>/dev/null || true
cp /etc/rabbitmq/certs/rabbitmq-server.key /etc/stunnel/certs/server.key 2>/dev/null || true
cp /etc/rabbitmq/certs/ca.crt            /etc/stunnel/certs/ca.crt     2>/dev/null || true
chown -R stunnel4:stunnel4 /etc/stunnel/certs
chmod 600 /etc/stunnel/certs/server.key 2>/dev/null || true
chmod 644 /etc/stunnel/certs/*.crt 2>/dev/null || true
after=$(fingerprint /etc/rabbitmq/certs)
if [ "$before" != "$after" ]; then
  echo "[sync] certs changed -> restart rabbitmq + stunnel"
  systemctl restart rabbitmq-server
  systemctl restart stunnel4
fi

# 4. Optional: keep the controller binary current.
if [ "$SYNC_BINARY" = "1" ]; then
  rsync -a -e "$RSYNC_RSH" "${SRC}:/root/controller-v4m" /root/controller-v4m 2>/dev/null || true
  chmod +x /root/controller-v4m 2>/dev/null || true
fi

# 5. Re-assert stand-by posture (disabled, stopped).
systemctl disable controller-v4m 2>/dev/null || true

echo "[sync] done"
