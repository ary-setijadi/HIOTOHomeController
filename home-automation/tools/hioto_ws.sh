#!/bin/sh
# Look for WebSocket/hub message structure strings in the binary.
B=/home/orangepi/hioto/main
echo "=== ws/hub/send strings ==="
strings -n 4 "$B" | grep -iE 'hub|websocket|SendMessage|/api/ws|/ws|wsMessage|Message' | grep -viE 'google|grpc|protobuf|envoy|ReadMessage|WriteMessage|websocket/proxy' | sort -u | head -60
echo "=== likely JSON message keys (short, snake/camel) ==="
strings -n 4 "$B" | grep -E '^[a-z_]{2,20}$' | sort -u | head -80
