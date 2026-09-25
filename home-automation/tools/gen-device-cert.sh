#!/bin/bash
# Issue a per-device client certificate signed by the HomeAutomation CA.
# Used for mTLS device onboarding (ESP32 and any TLS-capable device).
#
# Usage: gen-device-cert.sh <name> [certdir]
#   <name>    device identity (CN), e.g. ESP32-KITCHEN-01
#   [certdir] CA + output directory (default /etc/rabbitmq/certs)
set -euo pipefail

NAME="${1:-}"
CERTDIR="${2:-/etc/rabbitmq/certs}"

if [ -z "$NAME" ]; then
  echo "usage: $0 <device-name> [certdir]" >&2
  exit 1
fi

cd "$CERTDIR"

if [ ! -f ca.crt ] || [ ! -f ca.key ]; then
  echo "CA not found in $CERTDIR (need ca.crt + ca.key)" >&2
  exit 1
fi

echo "=== issuing client cert for '$NAME' ==="
openssl genrsa -out "$NAME.key" 2048 2>/dev/null
openssl req -new -key "$NAME.key" -subj "/CN=$NAME" -out "$NAME.csr"
printf 'extendedKeyUsage=clientAuth\nkeyUsage=digitalSignature\n' > "$NAME.ext"
openssl x509 -req -in "$NAME.csr" -CA ca.crt -CAkey ca.key -CAcreateserial \
  -days 3650 -sha256 -out "$NAME.crt" -extfile "$NAME.ext"
rm -f "$NAME.csr" "$NAME.ext"

chmod 600 "$NAME.key"
chmod 644 "$NAME.crt"

echo "done: $CERTDIR/$NAME.crt + $NAME.key (CN=$NAME)"
echo "ship to device: $NAME.crt + $NAME.key + ca.crt  (never share ca.key)"
