#!/bin/sh
# Extract likely HTTP route paths from the HIOTO `main` binary.
B=/home/orangepi/hioto/main
echo "=== short strings starting with / (candidate routes) ==="
strings -n 4 "$B" | grep '^/' | awk 'length($0) < 60' | sort -u
