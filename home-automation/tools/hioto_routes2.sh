#!/bin/sh
B=/home/orangepi/hioto/main
strings -n 8 "$B" | grep '^/' | awk 'length($0) >= 8 && length($0) < 55' \
  | grep -iE 'api|v1|wrapper|device|rule|floor|room|login|auth|control|monitor|alert|camera|sync|bell|user|register|ws|token|status' \
  | sort -u
