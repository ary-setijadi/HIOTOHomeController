#!/bin/sh
nm /home/orangepi/hioto/main | awk '{print $3}' | grep '^main\.' | sort -u
