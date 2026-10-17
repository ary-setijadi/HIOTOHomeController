#!/usr/bin/env python3
"""Find the freshly flashed Armbian board on the LAN.

The new card has no FAT boot partition, so nothing could be pre-staged on it --
the board comes up on DHCP and has to be discovered. This scans subnets for
open SSH, reads the ARP table for MACs, and (optionally) confirms which host is
the fresh board by trying Armbian's default root/1234 login.

Usage:
  # baseline BEFORE the board boots
  python find_board.py --subnets 192.168.1.0/24 192.168.31.0/24 --baseline baseline.json

  # after the board boots -- shows new hosts and probes them
  python find_board.py --subnets 192.168.1.0/24 192.168.31.0/24 \
      --baseline baseline.json --probe-default-login
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import ipaddress
import json
import os
import socket
import subprocess
import sys
import time

# Live, known infrastructure -- never probe these with the default password.
KNOWN = {"192.168.1.22", "192.168.1.23", "192.168.1.99"}


def scan_host(ip: str, port: int, timeout: float):
    try:
        with socket.create_connection((ip, port), timeout=timeout) as s:
            s.settimeout(timeout)
            try:
                banner = s.recv(128).decode("utf-8", "replace").strip()
            except Exception:
                banner = ""
        return ip, True, banner
    except Exception:
        return ip, False, ""


def arp_table() -> dict:
    out = {}
    try:
        txt = subprocess.run(["arp", "-a"], capture_output=True, text=True, timeout=30).stdout
    except Exception:
        return out
    for line in txt.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            ip = parts[0]
            try:
                ipaddress.ip_address(ip)
            except ValueError:
                continue
            mac = parts[1]
            if "-" in mac or ":" in mac:
                out[ip] = mac
    return out


def try_default_login(ip: str, timeout: float = 12.0):
    try:
        import paramiko
    except ImportError:
        return None
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        c.connect(ip, port=22, username="root", password="1234",
                  look_for_keys=False, allow_agent=False, timeout=timeout,
                  banner_timeout=timeout, auth_timeout=timeout)
        out = ""
        try:
            _, so, _ = c.exec_command(
                "cat /proc/device-tree/model 2>/dev/null | tr -d '\\0'; echo; "
                "hostname; uname -m", timeout=15)
            out = so.read().decode("utf-8", "replace").strip()
        except Exception:
            pass
        return out or "(connected)"
    except Exception:
        return None
    finally:
        c.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--subnets", nargs="+", required=True)
    ap.add_argument("--port", type=int, default=22)
    ap.add_argument("--timeout", type=float, default=0.8)
    ap.add_argument("--workers", type=int, default=256)
    ap.add_argument("--baseline", default=None, help="JSON file of previous findings")
    ap.add_argument("--probe-default-login", action="store_true")
    ap.add_argument("--exclude", nargs="*", default=[])
    args = ap.parse_args()

    hosts: list[str] = []
    for sn in args.subnets:
        net = ipaddress.ip_network(sn, strict=False)
        hosts.extend(str(h) for h in net.hosts())

    print(f"scanning {len(hosts)} hosts on port {args.port} ...", flush=True)
    t0 = time.time()
    open_hosts: list[tuple[str, str]] = []
    with cf.ThreadPoolExecutor(max_workers=args.workers) as ex:
        futs = {ex.submit(scan_host, h, args.port, args.timeout): h for h in hosts}
        for f in cf.as_completed(futs):
            ip, is_open, banner = f.result()
            if is_open:
                open_hosts.append((ip, banner))
    open_hosts.sort(key=lambda t: tuple(int(x) for x in t[0].split(".")))

    arps = arp_table()
    exclude = set(args.exclude) | KNOWN

    print(f"\nfound {len(open_hosts)} hosts with port {args.port} open "
          f"({time.time()-t0:.1f}s):")
    print(f"  {'IP':<18} {'MAC':<18} {'KNOWN':<7} SSH banner")
    for ip, banner in open_hosts:
        mac = arps.get(ip, "-")
        known = "known" if ip in exclude else "NEW"
        print(f"  {ip:<18} {mac:<18} {known:<7} {banner[:52]}")

    results = {ip: {"mac": arps.get(ip, ""), "banner": banner} for ip, banner in open_hosts}

    baseline = {}
    if args.baseline and os.path.exists(args.baseline):
        with open(args.baseline) as fh:
            baseline = json.load(fh)
        new = sorted(set(results) - set(baseline))
        gone = sorted(set(baseline) - set(results))
        print(f"\nvs baseline {args.baseline}:")
        print(f"  new hosts    : {new if new else 'none'}")
        print(f"  disappeared  : {gone if gone else 'none'}")
    else:
        new = []

    if args.baseline and not os.path.exists(args.baseline):
        with open(args.baseline, "w") as fh:
            json.dump(results, fh, indent=2, sort_keys=True)
        print(f"\nwrote baseline -> {args.baseline}")
        return

    if args.baseline:
        with open(args.baseline, "w") as fh:
            json.dump(results, fh, indent=2, sort_keys=True)

    if args.probe_default_login:
        cands = [ip for ip, _ in open_hosts if ip not in exclude]
        if not cands:
            print("\nno non-known SSH hosts to probe")
        else:
            print(f"\nprobing default Armbian login (root/1234) on {len(cands)} host(s) ...")
            for ip in cands:
                ident = try_default_login(ip)
                if ident:
                    print(f"  *** {ip} is the fresh Armbian board ***")
                    print("      " + ident.replace("\n", " | "))
                else:
                    print(f"  {ip}: not Armbian-default")


if __name__ == "__main__":
    sys.exit(main())
