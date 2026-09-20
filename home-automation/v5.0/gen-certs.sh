#!/bin/bash
set -e
CERTDIR=/etc/rabbitmq/certs
mkdir -p "$CERTDIR"
cd "$CERTDIR"

echo "=== generating CA ==="
openssl genrsa -out ca.key 2048 2>/dev/null
openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 -subj "/CN=HomeAutomation-CA" -out ca.crt

echo "=== generating server cert (broker) ==="
openssl genrsa -out server.key 2048 2>/dev/null
openssl req -new -key server.key -subj "/CN=maincontroller" -out server.csr
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 3650 -sha256 \
  -out server.crt \
  -extfile <(printf "subjectAltName=DNS:maincontroller,DNS:localhost,IP:127.0.0.1,IP:192.168.137.44")

echo "=== generating per-device client certs ==="
DEVICES="SNS-SW-001 SNS-SW-002 SNS-SW-003 SNS-SW-004 SNS-SW-005 SNS-SW-006 SNS-SW-007 ACT-LMP-001 ACT-LMP-002 ACT-LMP-003 ACT-LMP-004 ACT-LMP-005 ACT-LMP-006 ACT-PMP-001 SNS-PMP-001 SNS-TMP-001 SNS-TMP-002 SNS-TMP-003 SNS-TMP-004 SNS-AIR-001 SNS-FLW-001 ACT-AC-001 ACT-AC-002 ACT-AC-003 ACT-AC-004 ACT-APR-001 controller monitor house"
for dev in $DEVICES; do
  openssl genrsa -out "$dev.key" 2048 2>/dev/null
  openssl req -new -key "$dev.key" -subj "/CN=$dev" -out "$dev.csr"
  openssl x509 -req -in "$dev.csr" -CA ca.crt -CAkey ca.key -CAcreateserial -days 3650 -sha256 -out "$dev.crt"
done

rm -f ./*.csr
chown -R rabbitmq:rabbitmq "$CERTDIR"
chmod 600 ./*.key
chmod 644 ./*.crt
echo "=== done: $(ls *.crt | wc -l) certs in $CERTDIR ==="
ls *.crt | head -5
