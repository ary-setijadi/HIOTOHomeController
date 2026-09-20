#!/bin/sh
# Extract json struct tags (reveals request/response field names) from the binary.
B=/home/orangepi/hioto/main
strings -n 3 "$B" | grep 'json:' | sort -u
