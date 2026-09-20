#!/bin/sh
nm /home/orangepi/hioto/main | awk '{print $3}' | grep -i 'hioto' | grep -viE 'hioto-rmq|hioto_bell|hioto-bell' | sort -u | head -150
