#!/bin/sh
strings -n 4 /home/orangepi/hioto/main | grep '^/api' | sort -u
