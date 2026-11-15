#!/usr/bin/env python3
"""Capture live API responses as fixtures for the DOM harness."""
import http.cookiejar
import json
import sys
import urllib.request

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

BASE = "http://192.168.1.99:8082"
cj = http.cookiejar.CookieJar()
op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))
op.open(BASE + "/login", data=b"user=admin&password=Hioto-OTA-2026")

paths = [
    "/api/overview",
    "/api/channels",
    "/api/fleet",
    "/api/events?limit=300",
    "/api/health",
    "/api/channel?type=hioto-genmon-28-st7789",
]
fixtures = {}
for p in paths:
    fixtures[p] = json.load(op.open(BASE + p))
    print(f"captured {p}")

with open("_fixtures.json", "w", encoding="utf-8") as fh:
    json.dump(fixtures, fh)
print("wrote _fixtures.json")
