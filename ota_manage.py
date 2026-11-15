#!/usr/bin/env python3
"""HIOTO OTA management console (:8082).

A single-file, stdlib-only web console for the ESP32 OTA infrastructure that
already runs on this host:

    :8080  ota-firmware.service   device-facing OTA API   (untouched)
    :8081  monitor-server.service system/DNS dashboard    (untouched)
    :8082  ota-manage.service     THIS management console

What it manages
---------------
  * channels      one directory per device type under /srv/ota (<type>/latest.json
                  + <type>/firmware/*.bin)
  * publishing    upload a .bin, rename to the house convention
                  fw-<type>-<version>.bin, SHA-256 it and atomically rewrite
                  latest.json
  * promote       point a channel at any blob already on the server (rollback)
  * verify        recompute SHA-256 of the served blob and compare to latest.json
  * delete        remove a stale blob (never the live one)
  * fleet         per-GUID device state derived from /srv/ota/ota.log
                  (last seen, IP, reported version, up-to-date/outdated)
  * events        filtered view over the OTA log
  * system        service + disk + memory health and a live OTA self-test

Auth
----
Single operator account. PBKDF2-SHA256 password in /etc/ota-manage.json, session
cookie is HMAC-signed with a per-host secret. Mutating requests additionally
require the X-OTA-MGR header (blocks cross-site form posts) and the cookie is
SameSite=Strict.

Run:  python3 ota_manage.py [--port 8082] [--init-only]
"""
from __future__ import annotations

import hashlib
import hmac
import json
import os
import re
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from datetime import datetime, timezone
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, urlparse

# --------------------------------------------------------------------------- #
# configuration
# --------------------------------------------------------------------------- #

ROOT = "/srv/ota"                      # firmware root served by ota_server.py
LOG_FILE = os.path.join(ROOT, "ota.log")
CONFIG_PATH = "/etc/ota-manage.json"
OTA_PORT = 8080                        # the device-facing OTA server
OTA_SELF = f"http://127.0.0.1:{OTA_PORT}"
DEFAULT_PORT = 8082

MAX_UPLOAD = 32 * 1024 * 1024          # 32 MiB is far beyond any ESP32 image
SESSION_COOKIE = "ota_mgr"
SESSION_TTL = 12 * 3600
LOGIN_MAX_FAILS = 8                    # per IP, per window
LOGIN_WINDOW = 300

# Channels described in /srv/ota/CHANNELS.md -- used only to annotate the UI.
PRODUCTION_CHANNELS = {"hioto-genmon-28-st7789", "hioto-genmon-35-st7796"}
RETIRED_CHANNELS = {"hioto-genmon", "hioto-genmon-28", "hioto-genmon-35"}

DEVICE_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
VERSION_RE = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._+-]{0,31}$")
FILE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")


# --------------------------------------------------------------------------- #
# small helpers
# --------------------------------------------------------------------------- #

def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def ver_key(v):
    """Sort key for dotted versions; non-numeric parts sort as 0."""
    if not v:
        return (0,)
    parts = []
    for chunk in re.split(r"[._+-]", str(v)):
        parts.append(int(chunk) if chunk.isdigit() else 0)
    return tuple(parts) if parts else (0,)


def human_size(n) -> str:
    try:
        n = float(n)
    except (TypeError, ValueError):
        return "-"
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} GiB"


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_json(path: str, default=None):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except Exception:
        return default


def write_json_atomic(path: str, obj) -> None:
    """Write JSON via a temp file in the same directory, then os.replace()."""
    directory = os.path.dirname(path) or "."
    os.makedirs(directory, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=directory, prefix=".tmp-", suffix=".json")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            json.dump(obj, fh, indent=2)
            fh.write("\n")
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, path)
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def run_cmd(args, timeout=4) -> str:
    try:
        r = subprocess.run(args, capture_output=True, timeout=timeout, text=True)
        return (r.stdout or "").strip() or (r.stderr or "").strip()
    except Exception as exc:  # noqa: BLE001
        return f"error: {exc}"


def inside_root(*parts) -> str | None:
    """Join onto ROOT and refuse anything that escapes it."""
    candidate = os.path.realpath(os.path.join(ROOT, *parts))
    root = os.path.realpath(ROOT)
    if candidate != root and not candidate.startswith(root + os.sep):
        return None
    return candidate


def ota_base_url(host_header: str | None, fallback: str = "192.168.1.99") -> str:
    """Public base URL devices should pull from -- always the OTA port, not ours."""
    host = (host_header or "").split(":")[0].strip()
    if not host or not re.match(r"^[A-Za-z0-9.\-\[\]:]+$", host):
        host = fallback
    return f"http://{host}:{OTA_PORT}"


# --------------------------------------------------------------------------- #
# config / auth
# --------------------------------------------------------------------------- #

def hash_password(password: str) -> str:
    salt = secrets.token_hex(16)
    iterations = 200_000
    dk = hashlib.pbkdf2_hmac("sha256", password.encode(), salt.encode(), iterations)
    return f"pbkdf2_sha256${iterations}${salt}${dk.hex()}"


def check_password(password: str, stored: str) -> bool:
    try:
        algo, iters, salt, digest = stored.split("$")
        if algo != "pbkdf2_sha256":
            return False
        dk = hashlib.pbkdf2_hmac("sha256", password.encode(), salt.encode(), int(iters))
        return hmac.compare_digest(dk.hex(), digest)
    except Exception:
        return False


def load_config(create: bool = True) -> dict:
    cfg = read_json(CONFIG_PATH)
    if isinstance(cfg, dict) and cfg.get("password_hash") and cfg.get("secret"):
        cfg.setdefault("user", "admin")
        cfg.setdefault("port", DEFAULT_PORT)
        return cfg
    if not create:
        return {}
    initial = secrets.token_urlsafe(12)
    cfg = {
        "user": "admin",
        "password_hash": hash_password(initial),
        "secret": secrets.token_hex(32),
        "port": DEFAULT_PORT,
        "created": now_iso(),
    }
    write_json_atomic(CONFIG_PATH, cfg)
    try:
        os.chmod(CONFIG_PATH, 0o600)
    except OSError:
        pass
    print("=" * 62, flush=True)
    print("  OTA management console: created " + CONFIG_PATH, flush=True)
    print(f"  user:     {cfg['user']}", flush=True)
    print(f"  password: {initial}", flush=True)
    print("  Change it in the console under System -> Credentials.", flush=True)
    print("=" * 62, flush=True)
    return cfg


def sign_session(cfg: dict, issued: int) -> str:
    payload = f"{cfg.get('user', 'admin')}:{issued}"
    mac = hmac.new(cfg["secret"].encode(), payload.encode(), hashlib.sha256).hexdigest()
    return f"{issued}.{mac}"


def verify_session(cfg: dict, token: str) -> bool:
    try:
        issued_s, mac = token.split(".", 1)
        issued = int(issued_s)
    except Exception:
        return False
    if issued + SESSION_TTL < int(time.time()):
        return False
    payload = f"{cfg.get('user', 'admin')}:{issued}"
    expect = hmac.new(cfg["secret"].encode(), payload.encode(), hashlib.sha256).hexdigest()
    return hmac.compare_digest(mac, expect)


class LoginLimiter:
    def __init__(self) -> None:
        self._hits: dict[str, list[float]] = {}
        self._lock = threading.Lock()

    def blocked(self, ip: str) -> bool:
        now = time.time()
        with self._lock:
            hits = [t for t in self._hits.get(ip, []) if now - t < LOGIN_WINDOW]
            self._hits[ip] = hits
            return len(hits) >= LOGIN_MAX_FAILS

    def fail(self, ip: str) -> None:
        with self._lock:
            self._hits.setdefault(ip, []).append(time.time())

    def reset(self, ip: str) -> None:
        with self._lock:
            self._hits.pop(ip, None)


LIMITER = LoginLimiter()


# --------------------------------------------------------------------------- #
# domain: channels, fleet, events, health
# --------------------------------------------------------------------------- #

def channel_kind(device: str) -> str:
    if device in PRODUCTION_CHANNELS:
        return "production"
    if device in RETIRED_CHANNELS:
        return "retired"
    return "other"


def list_channels() -> list[str]:
    out = []
    try:
        for name in sorted(os.listdir(ROOT)):
            if os.path.isfile(os.path.join(ROOT, name, "latest.json")):
                out.append(name)
    except FileNotFoundError:
        pass
    return out


def read_manifest(device: str) -> dict | None:
    if not DEVICE_RE.match(device or ""):
        return None
    path = inside_root(device, "latest.json")
    if not path or not os.path.isfile(path):
        return None
    data = read_json(path)
    return data if isinstance(data, dict) else None


def channel_history(device: str, with_hashes: bool = False) -> list[dict]:
    """Blobs present in <device>/firmware, newest first."""
    fw_dir = inside_root(device, "firmware")
    if not fw_dir or not os.path.isdir(fw_dir):
        return []
    manifest = read_manifest(device) or {}
    live = manifest.get("filename")
    entries = []
    for name in os.listdir(fw_dir):
        if not FILE_RE.match(name):
            continue
        full = os.path.join(fw_dir, name)
        if not os.path.isfile(full):
            continue
        try:
            st = os.stat(full)
        except OSError:
            continue
        version = None
        m = re.match(r"^fw-" + re.escape(device) + r"-(.+)\.bin$", name)
        if m:
            version = m.group(1)
        entry = {
            "filename": name,
            "size": st.st_size,
            "mtime": datetime.fromtimestamp(st.st_mtime, timezone.utc).isoformat(),
            "version": version,
            "live": name == live,
        }
        if with_hashes:
            try:
                entry["sha256"] = sha256_file(full)
            except OSError:
                entry["sha256"] = None
            entry["sha256_matches_live"] = (
                entry["sha256"] == manifest.get("sha256") if entry["live"] else None
            )
        entries.append(entry)
    entries.sort(key=lambda e: (ver_key(e.get("version")), e["mtime"]), reverse=True)
    return entries


def channels_overview(with_history: bool = True) -> list[dict]:
    out = []
    for device in list_channels():
        manifest = read_manifest(device) or {}
        item = {
            "device_type": device,
            "kind": channel_kind(device),
            "manifest": manifest,
            "latest_version": manifest.get("latest_version"),
            "filename": manifest.get("filename"),
            "size": manifest.get("size"),
            "sha256": manifest.get("sha256"),
            "released_at": manifest.get("released_at"),
            "note": manifest.get("note") or "",
            "url": manifest.get("url"),
        }
        if with_history:
            hist = channel_history(device)
            item["history"] = hist
            item["version_count"] = len(hist)
            item["blob_bytes"] = sum(h["size"] for h in hist)
            live_file = inside_root(device, "firmware", manifest.get("filename") or "")
            item["live_present"] = bool(live_file and os.path.isfile(live_file))
        out.append(item)
    out.sort(key=lambda c: ({"production": 0, "other": 1, "retired": 2}[c["kind"]],
                            c["device_type"]))
    return out


def read_events(limit: int = 500, event: str = "", device: str = "",
                text: str = "", real_only: bool = False) -> list[dict]:
    if not os.path.isfile(LOG_FILE):
        return []
    # Keep the tail bounded: read at most ~4k lines from the end.
    try:
        with open(LOG_FILE, "rb") as fh:
            fh.seek(0, os.SEEK_END)
            size = fh.tell()
            block = min(size, 1 << 22)          # last 4 MiB
            fh.seek(size - block)
            raw = fh.read().decode("utf-8", "replace")
        lines = raw.splitlines()
        if block < size and lines:
            lines = lines[1:]                    # drop the partial first line
    except OSError:
        return []

    events = []
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            ev = json.loads(line)
        except Exception:
            continue
        if not isinstance(ev, dict):
            continue
        if event and ev.get("event") != event:
            continue
        if device and ev.get("device_type") != device:
            continue
        if real_only and not is_real_device(ev):
            continue
        if text:
            blob = " ".join(str(ev.get(k) or "") for k in
                            ("guid", "device_type", "ip", "message", "filename",
                             "status", "from_version", "to_version", "action"))
            if text.lower() not in blob.lower():
                continue
        events.append(ev)
    events.reverse()
    return events[:limit]


def is_real_device(ev: dict) -> bool:
    """Separate the ESP32 fleet from curl/publish probes."""
    ua = str(ev.get("user_agent") or "")
    guid = str(ev.get("guid") or "")
    if "ESP32" in ua or "esp32" in ua:
        return True
    if guid.startswith("hioto-genmon") or guid.startswith("esp32"):
        return True
    return False


def build_fleet() -> dict:
    """Per-GUID state, derived from ota.log plus the current manifests."""
    if not os.path.isfile(LOG_FILE):
        return {"devices": [], "counts": {}}
    try:
        with open(LOG_FILE, "rb") as fh:
            fh.seek(0, os.SEEK_END)
            size = fh.tell()
            block = min(size, 8 << 20)
            fh.seek(size - block)
            raw = fh.read().decode("utf-8", "replace")
        lines = raw.splitlines()
        if block < size and lines:
            lines = lines[1:]
    except OSError:
        return {"devices": [], "counts": {}}

    manifests = {d: (read_manifest(d) or {}) for d in list_channels()}
    latest_ver = {d: m.get("latest_version") for d, m in manifests.items()}

    fleet: dict[str, dict] = {}
    probes = {"queries": 0, "downloads": 0, "reports": 0}

    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            ev = json.loads(line)
        except Exception:
            continue
        if not isinstance(ev, dict):
            continue
        kind = ev.get("event")
        if kind not in ("query", "download", "report"):
            continue
        guid = str(ev.get("guid") or "").strip()
        if not guid:
            continue
        if not is_real_device(ev):
            key = f"probe:{guid or '-'}"
            if key not in fleet:
                fleet[key] = {"guid": guid or "-", "synthetic": True,
                              "device_type": None, "ip": None, "last_seen": 0,
                              "queries": 0, "downloads": 0, "reports": 0,
                              "agent": ev.get("user_agent") or ""}
            rec = fleet[key]
            rec[f"{kind}s" if kind != "query" else "queries"] += 1
            rec["last_seen"] = max(rec["last_seen"], ev.get("ts") or 0)
            probes["queries" if kind == "query" else
                   ("downloads" if kind == "download" else "reports")] += 1
            continue

        rec = fleet.get(guid)
        if rec is None:
            rec = fleet[guid] = {
                "guid": guid, "synthetic": False, "device_type": None, "ip": None,
                "last_seen": 0, "queries": 0, "downloads": 0, "reports": 0,
                "agent": ev.get("user_agent") or "", "current_version": None,
                "last_status": None, "last_report_at": None, "last_download": None,
                "from_version": None, "to_version": None, "message": None,
            }
        ts = ev.get("ts") or 0
        if ts >= (rec["last_seen"] or 0):
            if ev.get("device_type"):
                rec["device_type"] = ev["device_type"]
            if ev.get("ip"):
                rec["ip"] = ev["ip"]
            if ev.get("user_agent"):
                rec["agent"] = ev["user_agent"]
            rec["last_seen"] = ts

        if kind == "query":
            rec["queries"] += 1
        elif kind == "download":
            rec["downloads"] += 1
            if ts >= (rec.get("_dl_ts") or 0):
                rec["_dl_ts"] = ts
                rec["last_download"] = ev.get("filename")
        else:
            rec["reports"] += 1
            if ts >= (rec.get("_rep_ts") or 0):
                rec["_rep_ts"] = ts
                rec["last_status"] = ev.get("status")
                rec["from_version"] = ev.get("from_version")
                rec["to_version"] = ev.get("to_version")
                rec["message"] = ev.get("message")
                rec["last_report_at"] = ev.get("time")
                if ev.get("status") == "success" and ev.get("to_version"):
                    rec["current_version"] = ev.get("to_version")

    devices = []
    counts = {"current": 0, "outdated": 0, "ahead": 0, "unknown": 0, "unreported": 0}
    now = time.time()
    for rec in fleet.values():
        for junk in ("_dl_ts", "_rep_ts"):
            rec.pop(junk, None)
        if rec.get("synthetic"):
            rec["state"] = "probe"
            rec["latest_version"] = None
            rec["seconds_since_seen"] = int(now - rec["last_seen"]) if rec["last_seen"] else None
            rec["online"] = bool(rec["last_seen"] and now - rec["last_seen"] < 900)
            devices.append(rec)
            continue
        dtype = rec.get("device_type")
        latest = latest_ver.get(dtype)
        cur = rec.get("current_version")
        if cur and latest:
            a, b = ver_key(cur), ver_key(latest)
            state = "current" if a == b else ("outdated" if a < b else "ahead")
        elif cur:
            state = "unknown"
        else:
            state = "unreported"
        rec["latest_version"] = latest
        rec["state"] = state
        rec["seconds_since_seen"] = int(now - rec["last_seen"]) if rec["last_seen"] else None
        rec["online"] = bool(rec["last_seen"] and now - rec["last_seen"] < 900)
        counts[state] = counts.get(state, 0) + 1
        devices.append(rec)

    devices.sort(key=lambda d: (d["synthetic"], -(d["last_seen"] or 0)))
    return {"devices": devices, "counts": counts, "probes": probes}


def health() -> dict:
    services = {}
    for unit in ("ota-firmware", "ota-manage", "monitor-server", "pihole-FTL"):
        services[unit] = run_cmd(["systemctl", "is-active", unit]) or "unknown"

    disk = {}
    try:
        usage = shutil.disk_usage("/")
        disk = {"total": usage.total, "used": usage.used, "free": usage.free,
                "percent": round(usage.used / usage.total * 100, 1)}
    except OSError:
        pass

    mem = {}
    try:
        with open("/proc/meminfo", "r", encoding="utf-8") as fh:
            info = {}
            for line in fh:
                key, _, rest = line.partition(":")
                info[key] = int(rest.split()[0]) * 1024 if rest.split() else 0
        total = info.get("MemTotal", 0)
        avail = info.get("MemAvailable", 0)
        mem = {"total": total, "available": avail,
               "used": total - avail,
               "percent": round((total - avail) / total * 100, 1) if total else 0}
    except OSError:
        pass

    uptime_s = 0
    try:
        with open("/proc/uptime", "r", encoding="utf-8") as fh:
            uptime_s = int(float(fh.read().split()[0]))
    except Exception:
        pass

    ota_selftest = {"ok": False, "detail": "unreachable"}
    try:
        with urllib.request.urlopen(f"{OTA_SELF}/api/devices", timeout=4) as resp:
            payload = json.load(resp)
        devices = payload.get("devices", [])
        ota_selftest = {"ok": True, "detail": f"{len(devices)} channels served",
                        "devices": devices}
    except Exception as exc:  # noqa: BLE001
        ota_selftest = {"ok": False, "detail": str(exc)[:160]}

    root_bytes = 0
    blob_count = 0
    for device in list_channels():
        fw_dir = inside_root(device, "firmware")
        if not fw_dir or not os.path.isdir(fw_dir):
            continue
        for name in os.listdir(fw_dir):
            full = os.path.join(fw_dir, name)
            if os.path.isfile(full):
                try:
                    root_bytes += os.stat(full).st_size
                    blob_count += 1
                except OSError:
                    pass

    log_bytes = os.path.getsize(LOG_FILE) if os.path.isfile(LOG_FILE) else 0

    return {
        "host": socket.gethostname(),
        "uptime_seconds": uptime_s,
        "services": services,
        "disk": disk,
        "memory": mem,
        "ota_selftest": ota_selftest,
        "storage": {"firmware_root_bytes": root_bytes, "blob_count": blob_count,
                    "log_bytes": log_bytes},
        "time": now_iso(),
    }


def overview() -> dict:
    channels = channels_overview(with_history=True)
    fleet = build_fleet()
    events = read_events(limit=12)
    day_ago = time.time() - 86400
    recent = read_events(limit=4000)
    d24 = {"queries": 0, "downloads": 0, "reports": 0, "failed": 0}
    for ev in recent:
        if (ev.get("ts") or 0) < day_ago:
            continue
        kind = ev.get("event")
        if kind == "query":
            d24["queries"] += 1
        elif kind == "download":
            d24["downloads"] += 1
        elif kind == "report":
            d24["reports"] += 1
            if ev.get("status") not in (None, "success"):
                d24["failed"] += 1
    real = [d for d in fleet["devices"] if not d.get("synthetic")]
    return {
        "channels": channels,
        "fleet_counts": fleet.get("counts", {}),
        "probes": fleet.get("probes", {}),
        "device_total": len(real),
        "online": sum(1 for d in real if d.get("online")),
        "last24h": d24,
        "recent_events": events,
        "health": health(),
        "time": now_iso(),
    }


# --------------------------------------------------------------------------- #
# publish / promote / verify / delete
# --------------------------------------------------------------------------- #

def publish_blob(device: str, version: str, original_name: str, data: bytes,
                 note: str = "", base_url: str = "") -> dict:
    """Write the blob under the house filename and rewrite latest.json."""
    fw_dir = inside_root(device, "firmware")
    if not fw_dir:
        raise ValueError("invalid device type")
    os.makedirs(fw_dir, exist_ok=True)

    filename = f"fw-{device}-{version}.bin"
    if not FILE_RE.match(filename):
        raise ValueError("derived filename is not safe")

    final = os.path.join(fw_dir, filename)
    fd, tmp = tempfile.mkstemp(dir=fw_dir, prefix=".up-")
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        digest = sha256_file(tmp)
        size = os.path.getsize(tmp)
        os.replace(tmp, final)
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    try:
        os.chmod(final, 0o644)
    except OSError:
        pass

    manifest = {
        "device_type": device,
        "latest_version": version,
        "filename": filename,
        "url": (base_url.rstrip("/") if base_url else "http://192.168.1.99:8080")
               + f"/firmware/{device}/{filename}",
        "size": size,
        "sha256": digest,
        "released_at": now_iso(),
        "note": note or "",
    }
    write_json_atomic(os.path.join(ROOT, device, "latest.json"), manifest)
    return {"manifest": manifest, "original_name": original_name,
            "renamed_from": original_name if original_name != filename else None}


def promote_blob(device: str, filename: str, version: str = "", note: str = "",
                 base_url: str = "") -> dict:
    """Point a channel at a blob that already exists on the server (rollback)."""
    full = inside_root(device, "firmware", filename)
    if not full or not os.path.isfile(full):
        raise ValueError("blob not found in that channel")
    if not DEVICE_RE.match(device):
        raise ValueError("invalid device type")

    if not version:
        m = re.match(r"^fw-" + re.escape(device) + r"-(.+)\.bin$", filename)
        version = m.group(1) if m else ""
    if not VERSION_RE.match(version or ""):
        raise ValueError("could not derive a valid version; pass one explicitly")

    digest = sha256_file(full)
    size = os.path.getsize(full)
    previous = read_manifest(device) or {}
    manifest = {
        "device_type": device,
        "latest_version": version,
        "filename": filename,
        "url": (base_url.rstrip("/") if base_url else "http://192.168.1.99:8080")
               + f"/firmware/{device}/{filename}",
        "size": size,
        "sha256": digest,
        "released_at": now_iso(),
        "note": note or f"promoted from {previous.get('latest_version') or 'history'}",
    }
    write_json_atomic(os.path.join(ROOT, device, "latest.json"), manifest)
    return {"manifest": manifest, "previous": previous}


def verify_blob(device: str) -> dict:
    manifest = read_manifest(device)
    if not manifest:
        raise ValueError("channel has no manifest")
    filename = manifest.get("filename") or ""
    full = inside_root(device, "firmware", filename)
    if not full or not os.path.isfile(full):
        return {"ok": False, "reason": "served blob is missing on disk",
                "expected_sha256": manifest.get("sha256"), "filename": filename}
    actual = sha256_file(full)
    size = os.path.getsize(full)
    return {
        "ok": actual == manifest.get("sha256") and size == manifest.get("size"),
        "filename": filename,
        "expected_sha256": manifest.get("sha256"),
        "actual_sha256": actual,
        "expected_size": manifest.get("size"),
        "actual_size": size,
        "reason": "checksum and size match" if actual == manifest.get("sha256")
                  else "checksum mismatch",
    }


def delete_blob(device: str, filename: str) -> dict:
    manifest = read_manifest(device) or {}
    if filename == manifest.get("filename"):
        raise ValueError("refusing to delete the blob this channel currently serves")
    full = inside_root(device, "firmware", filename)
    if not full or not os.path.isfile(full):
        raise ValueError("blob not found")
    size = os.path.getsize(full)
    os.unlink(full)
    return {"deleted": filename, "freed_bytes": size}


def update_note(device: str, note: str) -> dict:
    manifest = read_manifest(device)
    if not manifest:
        raise ValueError("channel has no manifest")
    manifest["note"] = note[:400]
    write_json_atomic(os.path.join(ROOT, device, "latest.json"), manifest)
    return manifest


# --------------------------------------------------------------------------- #
# multipart (upload) parsing
# --------------------------------------------------------------------------- #

def parse_multipart(body: bytes, content_type: str) -> dict:
    m = re.search(r'boundary=("?)([^";]+)\1', content_type or "")
    if not m:
        return {}
    boundary = b"--" + m.group(2).encode()
    parts: dict[str, dict] = {}
    for chunk in body.split(boundary):
        if not chunk or chunk.strip() == b"--":
            continue
        if chunk.startswith(b"\r\n"):
            chunk = chunk[2:]
        if chunk.endswith(b"\r\n"):
            chunk = chunk[:-2]
        if chunk.startswith(b"--"):
            continue
        head, sep, data = chunk.partition(b"\r\n\r\n")
        if not sep:
            continue
        disposition = ""
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-disposition:"):
                disposition = line.decode("utf-8", "replace")
        nm = re.search(r'name="([^"]*)"', disposition)
        if not nm:
            continue
        fn = re.search(r'filename="([^"]*)"', disposition)
        if fn:
            parts[nm.group(1)] = {"filename": fn.group(1), "data": data}
        else:
            parts[nm.group(1)] = {"value": data.decode("utf-8", "replace")}
    return parts


# --------------------------------------------------------------------------- #
# HTTP layer
# --------------------------------------------------------------------------- #

class Handler(BaseHTTPRequestHandler):
    server_version = "OTAManage/1.0"
    protocol_version = "HTTP/1.1"

    # ---- plumbing -------------------------------------------------------- #

    def log_message(self, fmt, *args):
        sys.stdout.write("[ota-manage] %s - %s\n" % (self.address_string(), fmt % args))
        sys.stdout.flush()

    def _cfg(self) -> dict:
        return self.server.cfg  # type: ignore[attr-defined]

    def _send(self, code: int, body: bytes, ctype: str, extra: dict | None = None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "same-origin")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
            "img-src 'self' data:; connect-src 'self'; form-action 'self'; "
            "frame-ancestors 'none'; base-uri 'none'",
        )
        for key, value in (extra or {}).items():
            self.send_header(key, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _json(self, code: int, obj, extra: dict | None = None):
        body = json.dumps(obj, default=str).encode("utf-8")
        headers = {"Cache-Control": "no-store"}
        headers.update(extra or {})
        self._send(code, body, "application/json; charset=utf-8", headers)

    def _html(self, code: int, html: str, extra: dict | None = None):
        self._send(code, html.encode("utf-8"), "text/html; charset=utf-8", extra)

    def _redirect(self, location: str, extra: dict | None = None):
        headers = {"Location": location}
        headers.update(extra or {})
        self._send(303, b"", "text/plain", headers)

    def _cookie_token(self) -> str:
        raw = self.headers.get("Cookie")
        if not raw:
            return ""
        try:
            jar = SimpleCookie(raw)
        except Exception:
            return ""
        morsel = jar.get(SESSION_COOKIE)
        return morsel.value if morsel else ""

    def _authed(self) -> bool:
        return verify_session(self._cfg(), self._cookie_token())

    def _body(self) -> bytes:
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return b""
        if length <= 0:
            return b""
        if length > MAX_UPLOAD:
            return b""
        return self.rfile.read(length)

    def _require_auth(self) -> bool:
        if self._authed():
            return True
        path = urlparse(self.path).path
        if path.startswith("/api/"):
            self._json(401, {"error": "authentication required"})
        else:
            self._redirect("/login")
        return False

    def _require_csrf(self) -> bool:
        if self.headers.get("X-OTA-MGR") == "1":
            return True
        self._json(403, {"error": "missing X-OTA-MGR header"})
        return False

    # ---- GET ------------------------------------------------------------- #

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        qs = parse_qs(parsed.query)

        if path == "/favicon.ico":
            self._send(204, b"", "image/x-icon")
            return
        if path == "/login":
            if self._authed():
                self._redirect("/")
            else:
                self._html(200, LOGIN_HTML)
            return
        if path == "/app.css":
            self._send(200, CSS.encode("utf-8"), "text/css; charset=utf-8",
                       {"Cache-Control": "public, max-age=600"})
            return
        if path == "/app.js":
            self._send(200, JS.encode("utf-8"), "application/javascript; charset=utf-8",
                       {"Cache-Control": "public, max-age=600"})
            return

        if not self._require_auth():
            return

        if path in ("/", "/index.html"):
            self._html(200, APP_HTML)
            return

        if path == "/api/overview":
            self._json(200, overview())
            return

        if path == "/api/channels":
            self._json(200, {"channels": channels_overview(with_history=True)})
            return

        if path == "/api/channel":
            device = (qs.get("type") or [""])[0]
            manifest = read_manifest(device)
            if not manifest:
                self._json(404, {"error": "unknown channel"})
                return
            hist = channel_history(device, with_hashes=True)
            for entry in hist:
                if entry["live"]:
                    entry["sha256_matches_live"] = (
                        entry.get("sha256") == manifest.get("sha256"))
            self._json(200, {
                "device_type": device,
                "kind": channel_kind(device),
                "manifest": manifest,
                "history": hist,
            })
            return

        if path == "/api/fleet":
            self._json(200, build_fleet())
            return

        if path == "/api/events":
            try:
                limit = int((qs.get("limit") or ["300"])[0])
            except ValueError:
                limit = 300
            limit = max(1, min(limit, 4000))
            events = read_events(
                limit=limit,
                event=(qs.get("event") or [""])[0],
                device=(qs.get("device") or [""])[0],
                text=(qs.get("q") or [""])[0],
                real_only=(qs.get("real") or ["0"])[0] == "1",
            )
            self._json(200, {"count": len(events), "events": events})
            return

        if path == "/api/health":
            self._json(200, health())
            return

        if path == "/api/download":
            device = (qs.get("type") or [""])[0]
            filename = (qs.get("file") or [""])[0]
            if not (DEVICE_RE.match(device) and FILE_RE.match(filename)):
                self._json(400, {"error": "bad request"})
                return
            full = inside_root(device, "firmware", filename)
            if not full or not os.path.isfile(full):
                self._json(404, {"error": "blob not found"})
                return
            size = os.path.getsize(full)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.send_header("Content-Disposition",
                             f'attachment; filename="{filename}"')
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            with open(full, "rb") as fh:
                for chunk in iter(lambda: fh.read(65536), b""):
                    self.wfile.write(chunk)
            return

        self._json(404, {"error": "not found"})

    # ---- POST ------------------------------------------------------------ #

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/login":
            ip = self.client_address[0]
            if LIMITER.blocked(ip):
                self._html(429, LOGIN_HTML.replace(
                    "<!--ERR-->",
                    "Too many failed attempts. Wait a few minutes."))
                return
            form = parse_qs(self._body().decode("utf-8", "replace"))
            user = (form.get("user") or [""])[0]
            password = (form.get("password") or [""])[0]
            cfg = self._cfg()
            if user == cfg.get("user") and check_password(password,
                                                         cfg["password_hash"]):
                LIMITER.reset(ip)
                token = sign_session(cfg, int(time.time()))
                cookie = (f"{SESSION_COOKIE}={token}; Path=/; HttpOnly; "
                          f"SameSite=Strict; Max-Age={SESSION_TTL}")
                self._redirect("/", {"Set-Cookie": cookie})
            else:
                LIMITER.fail(ip)
                self._html(401, LOGIN_HTML.replace(
                    "<!--ERR-->", "Invalid credentials."))
            return

        if path == "/logout":
            cookie = (f"{SESSION_COOKIE}=; Path=/; HttpOnly; SameSite=Strict; "
                      "Max-Age=0")
            self._redirect("/login", {"Set-Cookie": cookie})
            return

        if not self._authed():
            self._json(401, {"error": "authentication required"})
            return
        if not path.startswith("/api/"):
            self._json(404, {"error": "not found"})
            return

        content_type = self.headers.get("Content-Type") or ""

        # password change comes from a normal form post (no custom header)
        if path == "/api/password":
            form = parse_qs(self._body().decode("utf-8", "replace"))
            current = (form.get("current") or [""])[0]
            newpw = (form.get("new") or [""])[0]
            cfg = self._cfg()
            if not check_password(current, cfg["password_hash"]):
                self._json(403, {"error": "current password is wrong"})
                return
            if len(newpw) < 8:
                self._json(400, {"error": "new password must be at least 8 characters"})
                return
            cfg["password_hash"] = hash_password(newpw)
            cfg["password_changed"] = now_iso()
            write_json_atomic(CONFIG_PATH, cfg)
            self.server.cfg = cfg  # type: ignore[attr-defined]
            self._json(200, {"ok": True, "message": "password updated"})
            return

        if not self._require_csrf():
            return

        body = self._body()

        if path == "/api/publish":
            if "multipart/form-data" not in content_type:
                self._json(400, {"error": "expected multipart/form-data"})
                return
            parts = parse_multipart(body, content_type)
            device = (parts.get("device", {}).get("value") or "").strip().lower()
            version = (parts.get("version", {}).get("value") or "").strip()
            note = (parts.get("note", {}).get("value") or "").strip()
            confirm = (parts.get("confirm", {}).get("value") or "").strip() == "1"
            upload = parts.get("firmware") or {}
            blob = upload.get("data") or b""
            original = (upload.get("filename") or "").strip()

            if not DEVICE_RE.match(device):
                self._json(400, {"error": "invalid device type "
                                          "(lowercase letters, digits, . _ -)"})
                return
            if not VERSION_RE.match(version):
                self._json(400, {"error": "invalid version string"})
                return
            if not blob:
                self._json(400, {"error": "empty or missing firmware payload"})
                return

            # Board-safety guard from CHANNELS.md: an image for the wrong panel
            # leaves a healthy board with a blank screen.
            known = set(list_channels()) | PRODUCTION_CHANNELS | RETIRED_CHANNELS
            mentioned = {k for k in known if k in original and k != device}
            if mentioned and not confirm:
                self._json(409, {
                    "error": "filename references a different channel "
                             f"({', '.join(sorted(mentioned))}); target is {device}",
                    "requires_confirmation": True,
                })
                return

            try:
                result = publish_blob(device, version, original, blob, note,
                                      base_url=ota_base_url(self.headers.get("Host")))
            except Exception as exc:  # noqa: BLE001
                self._json(500, {"error": str(exc)})
                return
            self._json(200, {"ok": True, **result})
            return

        # JSON endpoints
        try:
            payload = json.loads(body.decode("utf-8")) if body else {}
        except Exception:
            self._json(400, {"error": "invalid JSON body"})
            return
        if not isinstance(payload, dict):
            self._json(400, {"error": "expected a JSON object"})
            return

        if path == "/api/promote":
            device = str(payload.get("device") or "").lower()
            filename = str(payload.get("file") or "")
            if not (DEVICE_RE.match(device) and FILE_RE.match(filename)):
                self._json(400, {"error": "bad device or filename"})
                return
            try:
                result = promote_blob(
                    device, filename,
                    version=str(payload.get("version") or ""),
                    note=str(payload.get("note") or ""),
                    base_url=ota_base_url(self.headers.get("Host")),
                )
            except Exception as exc:  # noqa: BLE001
                self._json(400, {"error": str(exc)})
                return
            self._json(200, {"ok": True, **result})
            return

        if path == "/api/verify":
            device = str(payload.get("device") or "").lower()
            if not DEVICE_RE.match(device):
                self._json(400, {"error": "bad device"})
                return
            try:
                self._json(200, verify_blob(device))
            except Exception as exc:  # noqa: BLE001
                self._json(400, {"error": str(exc)})
            return

        if path == "/api/delete":
            device = str(payload.get("device") or "").lower()
            filename = str(payload.get("file") or "")
            if not (DEVICE_RE.match(device) and FILE_RE.match(filename)):
                self._json(400, {"error": "bad device or filename"})
                return
            try:
                self._json(200, {"ok": True, **delete_blob(device, filename)})
            except Exception as exc:  # noqa: BLE001
                self._json(400, {"error": str(exc)})
            return

        if path == "/api/note":
            device = str(payload.get("device") or "").lower()
            if not DEVICE_RE.match(device):
                self._json(400, {"error": "bad device"})
                return
            try:
                self._json(200, {"ok": True,
                                 "manifest": update_note(device,
                                                         str(payload.get("note") or ""))})
            except Exception as exc:  # noqa: BLE001
                self._json(400, {"error": str(exc)})
            return

        self._json(404, {"error": "not found"})

    def do_HEAD(self):
        # HEAD is only meaningful for the cheap endpoints; never stream blobs.
        if urlparse(self.path).path == "/api/download":
            self._json(405, {"error": "method not allowed"})
            return
        self.do_GET()

    def do_OPTIONS(self):
        self._json(405, {"error": "method not allowed"})


# --------------------------------------------------------------------------- #
# front-end assets
# --------------------------------------------------------------------------- #

CSS = """
:root{
  --bg:#0e1524; --panel:#16202f; --panel2:#1d2a3d; --panel3:#0b1220;
  --text:#e6edf7; --muted:#8fa3bd; --border:#253349;
  --green:#2ecc71; --blue:#4aa3ff; --purple:#a78bfa; --teal:#2dd4bf;
  --red:#ff5c5c; --orange:#ffb454; --yellow:#f5d76e;
  --radius:12px; --mono:ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,monospace;
}
*{box-sizing:border-box}
html,body{margin:0;padding:0}
body{
  background:var(--bg); color:var(--text); min-height:100vh;
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Inter,sans-serif;
  font-size:14px; line-height:1.5; -webkit-font-smoothing:antialiased;
}
a{color:var(--blue);text-decoration:none}
a:hover{text-decoration:underline}
code,.mono{font-family:var(--mono);font-size:12.5px}

/* ---------- shell ---------- */
.top{
  position:sticky;top:0;z-index:20;display:flex;align-items:center;gap:18px;
  padding:0 20px;height:56px;background:rgba(14,21,36,.94);
  border-bottom:1px solid var(--border);backdrop-filter:blur(8px);
}
.brand{display:flex;align-items:center;gap:10px;font-weight:650;letter-spacing:.2px}
.brand .dot{width:9px;height:9px;border-radius:50%;background:var(--green);
  box-shadow:0 0 10px var(--green)}
.brand small{color:var(--muted);font-weight:400;font-size:11.5px}
nav{display:flex;gap:2px;margin-left:8px;flex-wrap:wrap}
nav button{
  background:none;border:0;color:var(--muted);padding:7px 13px;border-radius:9px;
  cursor:pointer;font-size:13px;font-weight:520;font-family:inherit;
}
nav button:hover{background:var(--panel2);color:var(--text)}
nav button.active{background:var(--panel2);color:var(--text);
  box-shadow:inset 0 0 0 1px var(--border)}
.spacer{flex:1}
.who{color:var(--muted);font-size:12px;display:flex;align-items:center;gap:10px}
.btn{
  display:inline-flex;align-items:center;gap:6px;padding:7px 13px;border-radius:9px;
  border:1px solid var(--border);background:var(--panel2);color:var(--text);
  cursor:pointer;font-size:13px;font-family:inherit;font-weight:520;
}
.btn:hover{border-color:#31445f;background:#22314a}
.btn.primary{background:linear-gradient(180deg,#2f7fe0,#2568bd);border-color:#3a7fd5}
.btn.primary:hover{filter:brightness(1.1)}
.btn.danger{color:var(--red);border-color:#48263a}
.btn.danger:hover{background:#2a1a26;border-color:#6b3350}
.btn.sm{padding:4px 9px;font-size:12px;border-radius:7px}
.btn:disabled{opacity:.45;cursor:not-allowed}

main{max-width:1340px;margin:0 auto;padding:22px 20px 64px}
h1{font-size:19px;margin:0 0 4px;font-weight:650}
h2{font-size:15px;margin:26px 0 12px;font-weight:600}
.sub{color:var(--muted);font-size:12.5px;margin:0 0 18px}

/* ---------- cards & grids ---------- */
.grid{display:grid;gap:14px}
.grid.c4{grid-template-columns:repeat(4,minmax(0,1fr))}
.grid.c3{grid-template-columns:repeat(3,minmax(0,1fr))}
.grid.c2{grid-template-columns:repeat(2,minmax(0,1fr))}
@media(max-width:1000px){.grid.c4{grid-template-columns:repeat(2,1fr)}
  .grid.c3,.grid.c2{grid-template-columns:1fr}}
@media(max-width:620px){.grid.c4{grid-template-columns:1fr}}
.card{
  background:var(--panel);border:1px solid var(--border);border-radius:var(--radius);
  padding:16px;
}
.card.pad0{padding:0;overflow:hidden}
.stat .label{color:var(--muted);font-size:11.5px;text-transform:uppercase;
  letter-spacing:.6px;font-weight:600}
.stat .value{font-size:26px;font-weight:660;margin-top:6px;line-height:1.15}
.stat .hint{color:var(--muted);font-size:12px;margin-top:4px}
.stat.green .value{color:var(--green)}
.stat.blue .value{color:var(--blue)}
.stat.orange .value{color:var(--orange)}
.stat.red .value{color:var(--red)}
.stat.teal .value{color:var(--teal)}
.stat.purple .value{color:var(--purple)}

/* ---------- tables ---------- */
table{width:100%;border-collapse:collapse;font-size:13px}
th{
  text-align:left;color:var(--muted);font-size:11px;text-transform:uppercase;
  letter-spacing:.55px;font-weight:640;padding:10px 14px;
  border-bottom:1px solid var(--border);background:var(--panel3);white-space:nowrap;
}
td{padding:10px 14px;border-bottom:1px solid rgba(37,51,73,.6);vertical-align:middle}
tr:last-child td{border-bottom:0}
tbody tr:hover{background:rgba(29,42,61,.55)}
.scroll{overflow-x:auto}

/* ---------- chips ---------- */
.chip{
  display:inline-flex;align-items:center;gap:5px;padding:2px 9px;border-radius:999px;
  font-size:11.5px;font-weight:600;border:1px solid transparent;white-space:nowrap;
}
.chip.green{background:rgba(46,204,113,.13);color:var(--green);
  border-color:rgba(46,204,113,.32)}
.chip.orange{background:rgba(255,180,84,.13);color:var(--orange);
  border-color:rgba(255,180,84,.32)}
.chip.red{background:rgba(255,92,92,.13);color:var(--red);
  border-color:rgba(255,92,92,.32)}
.chip.blue{background:rgba(74,163,255,.13);color:var(--blue);
  border-color:rgba(74,163,255,.32)}
.chip.grey{background:rgba(143,163,189,.12);color:var(--muted);
  border-color:rgba(143,163,189,.28)}
.chip.purple{background:rgba(167,139,250,.13);color:var(--purple);
  border-color:rgba(167,139,250,.3)}
.pill-live{font-size:10.5px;padding:1px 7px}

/* ---------- channel cards ---------- */
.chan{border:1px solid var(--border);border-radius:var(--radius);
  background:var(--panel);margin-bottom:14px;overflow:hidden}
.chan-head{display:flex;align-items:center;gap:12px;padding:14px 16px;cursor:pointer}
.chan-head:hover{background:rgba(29,42,61,.5)}
.chan-title{font-weight:620;font-family:var(--mono);font-size:13.5px}
.chan-meta{color:var(--muted);font-size:12px;display:flex;gap:14px;flex-wrap:wrap;
  margin-top:3px}
.chan-body{border-top:1px solid var(--border);padding:0;display:none}
.chan.open .chan-body{display:block}
.chev{transition:transform .15s;color:var(--muted)}
.chan.open .chev{transform:rotate(90deg)}
.kv{display:grid;grid-template-columns:150px 1fr;gap:8px 16px;padding:16px;
  font-size:12.5px}
.kv .k{color:var(--muted)}
.kv .v{font-family:var(--mono);font-size:12px;word-break:break-all}

/* ---------- forms ---------- */
label.f{display:block;margin-bottom:14px}
label.f span{display:block;color:var(--muted);font-size:11.5px;font-weight:600;
  text-transform:uppercase;letter-spacing:.55px;margin-bottom:6px}
input[type=text],input[type=password],input[type=file],select,textarea{
  width:100%;padding:9px 11px;border-radius:9px;border:1px solid var(--border);
  background:var(--panel3);color:var(--text);font-family:inherit;font-size:13px;
}
input:focus,select:focus,textarea:focus{
  outline:none;border-color:var(--blue);box-shadow:0 0 0 3px rgba(74,163,255,.14)}
input[type=file]{padding:7px}
textarea{resize:vertical;min-height:70px}
.hint{color:var(--muted);font-size:12px;margin-top:5px}
.row{display:flex;gap:12px;flex-wrap:wrap;align-items:flex-end}
.row>*{flex:1;min-width:150px}
.row .fixed{flex:0 0 auto;min-width:0}

/* ---------- misc ---------- */
.banner{padding:11px 14px;border-radius:10px;margin-bottom:16px;font-size:13px;
  border:1px solid;display:flex;gap:10px;align-items:flex-start}
.banner.warn{background:rgba(255,180,84,.1);border-color:rgba(255,180,84,.35);
  color:#ffd79a}
.banner.err{background:rgba(255,92,92,.1);border-color:rgba(255,92,92,.35);
  color:#ffb3b3}
.banner.ok{background:rgba(46,204,113,.1);border-color:rgba(46,204,113,.35);
  color:#a8ecc4}
.banner.info{background:rgba(74,163,255,.1);border-color:rgba(74,163,255,.35);
  color:#b8d8ff}
.empty{padding:34px;text-align:center;color:var(--muted);font-size:13px}
.bar{height:6px;border-radius:999px;background:var(--panel3);overflow:hidden;
  margin-top:8px}
.bar>i{display:block;height:100%;background:linear-gradient(90deg,var(--blue),var(--teal))}
.bar.warn>i{background:linear-gradient(90deg,var(--orange),var(--red))}
.toast{
  position:fixed;right:20px;bottom:20px;z-index:60;display:flex;flex-direction:column;
  gap:8px;max-width:420px;
}
.toast>div{
  padding:11px 15px;border-radius:10px;font-size:13px;border:1px solid;
  box-shadow:0 10px 30px rgba(0,0,0,.4);animation:slide .18s ease-out;
  background:var(--panel2);
}
@keyframes slide{from{transform:translateY(8px);opacity:0}to{transform:none;opacity:1}}
.toast .ok{border-color:rgba(46,204,113,.4);color:#a8ecc4}
.toast .err{border-color:rgba(255,92,92,.45);color:#ffb3b3}
.toast .info{border-color:var(--border);color:var(--text)}
.spin{display:inline-block;width:12px;height:12px;border:2px solid var(--muted);
  border-top-color:transparent;border-radius:50%;animation:rot .7s linear infinite}
@keyframes rot{to{transform:rotate(360deg)}}
.muted{color:var(--muted)}
.right{text-align:right}
.nowrap{white-space:nowrap}
.tiny{font-size:11.5px}
.trunc{max-width:340px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}

/* ---------- login ---------- */
.login-wrap{min-height:100vh;display:flex;align-items:center;justify-content:center;
  padding:20px}
.login-card{width:100%;max-width:390px;background:var(--panel);
  border:1px solid var(--border);border-radius:16px;padding:30px;
  box-shadow:0 24px 60px rgba(0,0,0,.45)}
.login-card h1{font-size:17px;margin:0 0 4px}
.login-card .sub{margin-bottom:22px}
.login-card .brand{margin-bottom:22px;font-size:15px}
"""


LOGIN_HTML = """<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sign in - OTA Console</title>
<link rel="stylesheet" href="/app.css">
</head><body>
<div class="login-wrap">
  <form class="login-card" method="post" action="/login" autocomplete="off">
    <div class="brand"><span class="dot"></span> HIOTO OTA Console</div>
    <h1>Operator sign-in</h1>
    <p class="sub">Firmware channels, fleet versions and publish control.</p>
    <!--ERR-->
    <label class="f"><span>User</span>
      <input type="text" name="user" value="admin" required autofocus></label>
    <label class="f"><span>Password</span>
      <input type="password" name="password" required></label>
    <button class="btn primary" type="submit" style="width:100%;justify-content:center">
      Sign in</button>
  </form>
</div>
</body></html>
"""


APP_HTML = """<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HIOTO OTA Console</title>
<link rel="stylesheet" href="/app.css">
</head><body>
<header class="top">
  <div class="brand"><span class="dot"></span> HIOTO OTA
    <small id="hostname"></small></div>
  <nav id="nav">
    <button data-view="overview" class="active">Overview</button>
    <button data-view="channels">Channels</button>
    <button data-view="fleet">Fleet</button>
    <button data-view="events">Events</button>
    <button data-view="publish">Publish</button>
    <button data-view="system">System</button>
  </nav>
  <div class="spacer"></div>
  <div class="who">
    <span id="clock" class="mono tiny"></span>
    <form method="post" action="/logout" style="margin:0">
      <button class="btn sm" type="submit">Sign out</button>
    </form>
  </div>
</header>
<main id="view"><div class="empty">Loading...</div></main>
<div class="toast" id="toast"></div>
<script src="/app.js"></script>
</body></html>
"""


JS = r"""
'use strict';

// ---------------------------------------------------------------- utilities
const $  = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

const esc = (v) => String(v === null || v === undefined ? '' : v)
  .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
  .replace(/"/g, '&quot;').replace(/'/g, '&#39;');

const fmtBytes = (n) => {
  n = Number(n);
  if (!isFinite(n) || n < 0) return '-';
  if (n < 1024) return n + ' B';
  const u = ['KiB', 'MiB', 'GiB', 'TiB'];
  let i = -1;
  do { n /= 1024; i++; } while (n >= 1024 && i < u.length - 1);
  return n.toFixed(1) + ' ' + u[i];
};

const fmtAgo = (ts) => {
  if (!ts) return 'never';
  const s = Math.max(0, Math.round(Date.now() / 1000 - Number(ts)));
  if (s < 60) return s + 's ago';
  if (s < 3600) return Math.round(s / 60) + 'm ago';
  if (s < 86400) return Math.round(s / 3600) + 'h ago';
  return Math.round(s / 86400) + 'd ago';
};

const fmtTime = (iso) => {
  if (!iso) return '-';
  const d = new Date(iso);
  if (isNaN(d)) return String(iso);
  return d.toLocaleString(undefined, {
    year: 'numeric', month: 'short', day: '2-digit',
    hour: '2-digit', minute: '2-digit', second: '2-digit'
  });
};

const shortSha = (s) => s ? String(s).slice(0, 12) + '...' : '-';

function toast(msg, kind = 'info') {
  const box = $('#toast');
  const el = document.createElement('div');
  el.className = kind;
  el.textContent = msg;
  box.appendChild(el);
  setTimeout(() => el.remove(), kind === 'err' ? 7000 : 4200);
}

async function api(path, opts = {}) {
  const init = { credentials: 'same-origin', headers: Object.assign({}, opts.headers) };
  if (opts.method && opts.method !== 'GET') init.headers['X-OTA-MGR'] = '1';
  if (opts.json !== undefined) {
    init.method = opts.method || 'POST';
    init.headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(opts.json);
  } else if (opts.body !== undefined) {
    init.method = opts.method || 'POST';
    init.body = opts.body;
  } else if (opts.method) {
    init.method = opts.method;
  }
  const res = await fetch(path, init);
  if (res.status === 401) { location.href = '/login'; throw new Error('unauthorized'); }
  const ct = res.headers.get('content-type') || '';
  const data = ct.includes('json') ? await res.json() : await res.text();
  if (!res.ok) {
    const msg = (data && data.error) ? data.error : ('HTTP ' + res.status);
    const err = new Error(msg);
    err.payload = data;
    throw err;
  }
  return data;
}

const state = { view: 'overview', channels: [], fleet: null, timer: null,
                openChannels: new Set(), filters: { event: '', device: '', q: '' } };

// ---------------------------------------------------------------- renderers
function chipState(s) {
  const map = {
    current:   ['green',  'up to date'],
    outdated:  ['orange', 'outdated'],
    ahead:     ['purple', 'ahead of channel'],
    unreported:['grey',   'no report yet'],
    unknown:   ['grey',   'unknown']
  };
  const [cls, label] = map[s] || ['grey', s || 'unknown'];
  return '<span class="chip ' + cls + '">' + esc(label) + '</span>';
}

function chipKind(k) {
  if (k === 'production') return '<span class="chip green pill-live">production</span>';
  if (k === 'retired') return '<span class="chip grey pill-live">retired</span>';
  return '<span class="chip blue pill-live">other</span>';
}

function serviceChip(name, status) {
  const ok = status === 'active';
  return '<div style="display:flex;justify-content:space-between;gap:12px;' +
         'padding:7px 0;border-bottom:1px solid rgba(37,51,73,.5)">' +
         '<span class="mono tiny">' + esc(name) + '</span>' +
         '<span class="chip ' + (ok ? 'green' : 'red') + '">' + esc(status || '?') +
         '</span></div>';
}

function renderOverview(d) {
  const h = d.health || {};
  const disk = h.disk || {}, mem = h.memory || {}, st = h.storage || {};
  const ls = h.ota_selftest || {};
  const c = d.fleet_counts || {};
  const day = d.last24h || {};

  const stat = (cls, label, value, hint) =>
    '<div class="card stat ' + cls + '"><div class="label">' + esc(label) + '</div>' +
    '<div class="value">' + value + '</div>' +
    '<div class="hint">' + (hint || '') + '</div></div>';

  const channels = (d.channels || []).filter(ch => ch.kind !== 'retired');
  const chanRows = channels.map(ch =>
    '<tr><td><span class="mono">' + esc(ch.device_type) + '</span> ' +
    chipKind(ch.kind) + '</td>' +
    '<td><span class="chip blue">v' + esc(ch.latest_version || '?') + '</span></td>' +
    '<td class="mono tiny">' + esc(ch.filename || '-') + '</td>' +
    '<td class="right mono tiny">' + fmtBytes(ch.size) + '</td>' +
    '<td class="mono tiny">' + shortSha(ch.sha256) + '</td>' +
    '<td class="tiny muted nowrap">' + fmtTime(ch.released_at) + '</td>' +
    '<td>' + (ch.live_present
        ? '<span class="chip green">blob ok</span>'
        : '<span class="chip red">missing</span>') + '</td></tr>').join('');

  const evRows = (d.recent_events || []).slice(0, 10).map(ev => {
    const kind = ev.event || '?';
    const cls = kind === 'report'
      ? (ev.status === 'success' ? 'green' : 'orange')
      : (kind === 'download' ? 'blue' : 'grey');
    const what = kind === 'report'
      ? (ev.from_version || '?') + ' &rarr; ' + (ev.to_version || '?') +
        (ev.status && ev.status !== 'success' ? ' (' + esc(ev.status) + ')' : '')
      : (kind === 'download' ? esc(ev.filename || '-') : 'polled /api/latest');
    return '<tr><td class="tiny muted nowrap">' + fmtTime(ev.time) + '</td>' +
      '<td><span class="chip ' + cls + '">' + esc(kind) + '</span></td>' +
      '<td class="mono tiny">' + esc(ev.device_type || '-') + '</td>' +
      '<td class="mono tiny">' + esc(ev.guid || '-') + '</td>' +
      '<td class="mono tiny">' + esc(ev.ip || '-') + '</td>' +
      '<td class="tiny trunc">' + what + '</td></tr>';
  }).join('');

  return '' +
  '<h1>Fleet overview</h1>' +
  '<p class="sub">Live state of the ESP32 OTA estate served from ' +
    '<span class="mono">/srv/ota</span>.</p>' +

  (ls.ok
    ? '<div class="banner ok"><b>OTA service healthy</b> &mdash; ' + esc(ls.detail) + '</div>'
    : '<div class="banner err"><b>OTA service unreachable</b> &mdash; ' +
      esc(ls.detail || 'no response from :8080') + '</div>') +

  '<div class="grid c4">' +
    stat('blue', 'Live channels', channels.length,
         c.retired ? '' : 'production + other') +
    stat('teal', 'Devices seen', d.device_total || 0,
         (d.online || 0) + ' online in the last 15 min') +
    stat((c.outdated ? 'orange' : 'green'), 'Up to date',
         (c.current || 0) + ' / ' + (d.device_total || 0),
         (c.outdated || 0) + ' outdated, ' + (c.unreported || 0) + ' unreported') +
    stat('purple', 'Downloads 24h', day.downloads || 0,
         (day.queries || 0) + ' polls, ' + (day.reports || 0) + ' reports') +
  '</div>' +

  '<h2>Channels in service</h2>' +
  '<div class="card pad0 scroll"><table><thead><tr>' +
    '<th>channel</th><th>version</th><th>filename</th><th class="right">size</th>' +
    '<th>sha256</th><th>released</th><th>served blob</th>' +
  '</tr></thead><tbody>' + (chanRows || '<tr><td colspan="7" class="empty">' +
    'No channels yet &mdash; publish a firmware image to begin.</td></tr>') +
  '</tbody></table></div>' +

  '<div class="grid c2" style="margin-top:22px">' +
    '<div class="card"><div class="label muted tiny" style="text-transform:uppercase;' +
      'letter-spacing:.6px;font-weight:600;font-size:11.5px">Recent activity</div>' +
      '<div class="scroll" style="margin-top:10px;margin-left:-16px;margin-right:-16px">' +
      '<table><tbody>' + (evRows || '<tr><td class="empty">No events yet.</td></tr>') +
      '</tbody></table></div></div>' +
    '<div class="card"><div class="label muted tiny" style="text-transform:uppercase;' +
      'letter-spacing:.6px;font-weight:600;font-size:11.5px">Host health</div>' +
      '<div style="margin-top:10px">' +
        Object.entries(h.services || {}).map(([k, v]) => serviceChip(k, v)).join('') +
        '<div style="margin-top:14px" class="tiny muted">Disk ' +
          fmtBytes(disk.used) + ' / ' + fmtBytes(disk.total) +
          ' &middot; ' + (disk.percent || 0) + '%</div>' +
        '<div class="bar ' + ((disk.percent || 0) > 85 ? 'warn' : '') + '">' +
          '<i style="width:' + Math.min(100, disk.percent || 0) + '%"></i></div>' +
        '<div style="margin-top:12px" class="tiny muted">Memory ' +
          fmtBytes(mem.used) + ' / ' + fmtBytes(mem.total) +
          ' &middot; ' + (mem.percent || 0) + '%</div>' +
        '<div class="bar ' + ((mem.percent || 0) > 90 ? 'warn' : '') + '">' +
          '<i style="width:' + Math.min(100, mem.percent || 0) + '%"></i></div>' +
        '<div style="margin-top:14px" class="tiny muted">' +
          (st.blob_count || 0) + ' firmware blobs, ' + fmtBytes(st.firmware_root_bytes) +
          ' &middot; log ' + fmtBytes(st.log_bytes) + '</div>' +
      '</div></div>' +
  '</div>';
}

function renderChannels() {
  const list = state.channels;
  if (!list.length) {
    return '<h1>Channels</h1><div class="card empty">No channels yet.</div>';
  }
  const cards = list.map(ch => {
    const open = state.openChannels.has(ch.device_type) ? ' open' : '';
    const hist = (ch.history || []).map(h =>
      '<tr><td class="mono tiny">' + esc(h.version || '-') + '</td>' +
      '<td class="mono tiny trunc">' + esc(h.filename) + '</td>' +
      '<td class="right mono tiny">' + fmtBytes(h.size) + '</td>' +
      '<td class="tiny muted nowrap">' + fmtTime(h.mtime) + '</td>' +
      '<td>' + (h.live ? '<span class="chip green pill-live">live</span>'
                       : '<span class="chip grey pill-live">history</span>') + '</td>' +
      '<td class="right nowrap">' +
        (h.live
          ? '<button class="btn sm" data-verify="' + esc(ch.device_type) + '">verify</button>'
          : '<button class="btn sm" data-promote="' + esc(ch.device_type) + '" ' +
            'data-file="' + esc(h.filename) + '" data-ver="' + esc(h.version || '') +
            '">promote</button> ') +
        (h.live ? '' :
          '<button class="btn sm danger" data-del="' + esc(ch.device_type) + '" ' +
          'data-file="' + esc(h.filename) + '">delete</button>') +
      '</td></tr>').join('');

    return '<div class="chan' + open + '" data-chan="' + esc(ch.device_type) + '">' +
      '<div class="chan-head" data-toggle="' + esc(ch.device_type) + '">' +
        '<span class="chev">&#9656;</span>' +
        '<div style="flex:1;min-width:0">' +
          '<div class="chan-title">' + esc(ch.device_type) + ' ' + chipKind(ch.kind) + '</div>' +
          '<div class="chan-meta">' +
            '<span>latest <b>v' + esc(ch.latest_version || '?') + '</b></span>' +
            '<span>' + fmtBytes(ch.size) + '</span>' +
            '<span>' + (ch.version_count || 0) + ' blobs, ' +
              fmtBytes(ch.blob_bytes) + '</span>' +
            '<span>' + fmtTime(ch.released_at) + '</span>' +
          '</div>' +
        '</div>' +
        (ch.live_present ? '<span class="chip green">blob ok</span>'
                         : '<span class="chip red">blob missing</span>') +
      '</div>' +
      '<div class="chan-body">' +
        '<div class="kv">' +
          '<div class="k">Served filename</div><div class="v">' +
            esc(ch.filename || '-') + '</div>' +
          '<div class="k">SHA-256</div><div class="v">' + esc(ch.sha256 || '-') + '</div>' +
          '<div class="k">Device pull URL</div><div class="v">' + esc(ch.url || '-') + '</div>' +
          '<div class="k">Note</div><div class="v" style="font-family:inherit">' +
            esc(ch.note || '(none)') + '</div>' +
        '</div>' +
        '<div class="scroll"><table><thead><tr><th>version</th><th>file</th>' +
          '<th class="right">size</th><th>built</th><th>state</th>' +
          '<th class="right">actions</th></tr></thead><tbody>' +
          (hist || '<tr><td colspan="6" class="empty">No blobs in this channel.</td></tr>') +
        '</tbody></table></div>' +
      '</div>' +
    '</div>';
  }).join('');

  return '<h1>Channels</h1>' +
    '<p class="sub">One channel per board + panel controller. A board can only ' +
    'pull the image compiled for its own display, so publishing to the wrong ' +
    'channel leaves a healthy board with a blank screen.</p>' +
    cards;
}

function renderFleet() {
  const f = state.fleet || { devices: [], counts: {} };
  const rows = f.devices.map(d => {
    const on = d.online
      ? '<span class="chip green pill-live">online</span>'
      : '<span class="chip grey pill-live">idle</span>';
    return '<tr><td class="mono tiny">' + esc(d.guid) + '</td>' +
      '<td class="mono tiny">' + esc(d.device_type || '-') + '</td>' +
      '<td>' + (d.current_version
        ? '<span class="chip blue">v' + esc(d.current_version) + '</span>'
        : '<span class="muted tiny">-</span>') + '</td>' +
      '<td class="mono tiny muted">' + (d.latest_version
        ? 'v' + esc(d.latest_version) : '-') + '</td>' +
      '<td>' + (d.synthetic ? '<span class="chip purple pill-live">probe</span> '
                            : chipState(d.state)) + '</td>' +
      '<td class="mono tiny">' + esc(d.ip || '-') + '</td>' +
      '<td class="tiny muted nowrap">' + fmtAgo(d.last_seen) + '</td>' +
      '<td>' + on + '</td>' +
      '<td class="right tiny muted">' + (d.queries || 0) + ' / ' +
        (d.downloads || 0) + ' / ' + (d.reports || 0) + '</td></tr>';
  }).join('');

  const c = f.counts || {};
  return '<h1>Fleet</h1>' +
    '<p class="sub">Per-device state reconstructed from the OTA request log ' +
    '(poll / download / report). Version comes from the last successful flash report.</p>' +
    '<div class="grid c4">' +
      '<div class="card stat green"><div class="label">Up to date</div>' +
        '<div class="value">' + (c.current || 0) + '</div></div>' +
      '<div class="card stat orange"><div class="label">Outdated</div>' +
        '<div class="value">' + (c.outdated || 0) + '</div></div>' +
      '<div class="card stat blue"><div class="label">No report</div>' +
        '<div class="value">' + (c.unreported || 0) + '</div></div>' +
      '<div class="card stat purple"><div class="label">Ahead / unknown</div>' +
        '<div class="value">' + ((c.ahead || 0) + (c.unknown || 0)) + '</div></div>' +
    '</div>' +
    '<h2>Devices</h2>' +
    '<div class="card pad0 scroll"><table><thead><tr><th>guid</th><th>channel</th>' +
      '<th>running</th><th>channel latest</th><th>state</th><th>ip</th>' +
      '<th>last seen</th><th>link</th><th class="right">poll/dl/report</th>' +
    '</tr></thead><tbody>' +
      (rows || '<tr><td colspan="9" class="empty">No devices have contacted the ' +
        'OTA server yet.</td></tr>') +
    '</tbody></table></div>';
}

function renderEvents() {
  const f = state.filters;
  const opts = ['', 'query', 'download', 'report'].map(v =>
    '<option value="' + v + '"' + (f.event === v ? ' selected' : '') + '>' +
    (v || 'all events') + '</option>').join('');
  const devs = state.channels.map(ch =>
    '<option value="' + esc(ch.device_type) + '"' +
    (f.device === ch.device_type ? ' selected' : '') + '>' +
    esc(ch.device_type) + '</option>').join('');

  return '<h1>Events</h1>' +
    '<p class="sub">Tail of <span class="mono">/srv/ota/ota.log</span>. ' +
    'Poll, download and post-flash report records.</p>' +
    '<div class="card" style="margin-bottom:16px"><div class="row">' +
      '<label class="f" style="margin:0"><span>Event type</span>' +
        '<select id="f-event">' + opts + '</select></label>' +
      '<label class="f" style="margin:0"><span>Channel</span>' +
        '<select id="f-device"><option value="">all channels</option>' + devs +
        '</select></label>' +
      '<label class="f" style="margin:0"><span>Search</span>' +
        '<input type="text" id="f-q" placeholder="guid, ip, version, note..." value="' +
        esc(f.q) + '"></label>' +
      '<div class="fixed"><button class="btn primary" id="f-apply">Refresh</button></div>' +
    '</div></div>' +
    '<div id="events-table"><div class="empty"><span class="spin"></span></div></div>';
}

function renderEventsTable(data) {
  const rows = (data.events || []).map(ev => {
    const kind = ev.event || '?';
    const cls = kind === 'report'
      ? (ev.status === 'success' ? 'green' : 'orange')
      : (kind === 'download' ? 'blue' : 'grey');
    return '<tr><td class="tiny muted nowrap">' + fmtTime(ev.time) + '</td>' +
      '<td><span class="chip ' + cls + '">' + esc(kind) + '</span></td>' +
      '<td class="mono tiny">' + esc(ev.device_type || '-') + '</td>' +
      '<td class="mono tiny">' + esc(ev.guid || '-') + '</td>' +
      '<td class="mono tiny">' + esc(ev.ip || '-') + '</td>' +
      '<td class="mono tiny">' + esc(ev.filename || '-') + '</td>' +
      '<td class="tiny">' + (ev.event === 'report'
        ? esc(ev.from_version || '?') + ' &rarr; ' + esc(ev.to_version || '?')
        : '-') + '</td>' +
      '<td class="tiny">' + (ev.status
        ? '<span class="chip ' + (ev.status === 'success' ? 'green' : 'red') + '">' +
          esc(ev.status) + '</span>' : '-') + '</td>' +
      '<td class="tiny muted trunc">' + esc(ev.message || '') + '</td></tr>';
  }).join('');

  return '<div class="card pad0 scroll"><table><thead><tr><th>time</th>' +
    '<th>event</th><th>channel</th><th>guid</th><th>ip</th><th>file</th>' +
    '<th>transition</th><th>status</th><th>message</th></tr></thead><tbody>' +
    (rows || '<tr><td colspan="9" class="empty">No matching events.</td></tr>') +
    '</tbody></table></div>' +
    '<div class="tiny muted" style="margin-top:10px">' + (data.count || 0) +
    ' matching records</div>';
}

function renderPublish() {
  const opts = state.channels.map(ch =>
    '<option value="' + esc(ch.device_type) + '">' + esc(ch.device_type) +
    ' (latest v' + esc(ch.latest_version || '?') + ')</option>').join('');
  return '<h1>Publish firmware</h1>' +
    '<p class="sub">The image is stored as <span class="mono">fw-&lt;channel&gt;' +
    '-&lt;version&gt;.bin</span>, SHA-256 hashed, and the channel manifest is ' +
    'rewritten atomically. Devices pick it up on their next poll.</p>' +
    '<div class="banner warn"><span>&#9888;</span><div><b>Panel safety.</b> A ' +
    '2.8&quot; ST7789 image and a 3.5&quot; ST7796 image are not interchangeable. ' +
    'Publishing the wrong one leaves the board running with a blank screen.</div></div>' +
    '<div class="card" style="max-width:720px">' +
      '<form id="pub-form">' +
        '<label class="f"><span>Target channel</span>' +
          '<select name="device" id="pub-device" required>' + opts + '</select>' +
          '<div class="hint">Or type a new channel name below to create one.</div>' +
          '<input type="text" id="pub-newchan" placeholder="new channel name (optional)" ' +
          'style="margin-top:8px"></label>' +
        '<label class="f"><span>Version</span>' +
          '<input type="text" name="version" id="pub-version" ' +
          'placeholder="e.g. 2.0.7" required></label>' +
        '<label class="f"><span>Firmware image (.bin)</span>' +
          '<input type="file" name="firmware" id="pub-file" accept=".bin,application/octet-stream" ' +
          'required></label>' +
        '<label class="f"><span>Release note</span>' +
          '<input type="text" name="note" id="pub-note" ' +
          'placeholder="what changed in this build"></label>' +
        '<label class="tiny" style="display:flex;gap:8px;align-items:flex-start;' +
          'margin-bottom:16px">' +
          '<input type="checkbox" id="pub-confirm" style="width:auto;margin-top:3px">' +
          '<span class="muted">Confirm this image is built for the selected channel ' +
          'panel (override the filename/channel mismatch check).</span></label>' +
        '<button class="btn primary" type="submit" id="pub-submit">Publish</button>' +
        '<span id="pub-status" class="tiny muted" style="margin-left:12px"></span>' +
      '</form>' +
      '<div id="pub-result" style="margin-top:16px"></div>' +
    '</div>';
}

function renderSystem(d) {
  const h = d || {};
  const disk = h.disk || {}, mem = h.memory || {}, st = h.storage || {};
  const up = h.uptime_seconds || 0;
  const upStr = Math.floor(up / 86400) + 'd ' + Math.floor((up % 86400) / 3600) + 'h ' +
                Math.floor((up % 3600) / 60) + 'm';
  const ls = h.ota_selftest || {};
  return '<h1>System</h1>' +
    '<p class="sub">Host and service health for the OTA infrastructure on ' +
    esc(h.host || '') + '.</p>' +
    '<div class="grid c3">' +
      '<div class="card"><div class="label muted tiny" style="text-transform:uppercase;' +
        'font-size:11.5px;letter-spacing:.6px;font-weight:600">Services</div>' +
        '<div style="margin-top:10px">' +
        Object.entries(h.services || {}).map(([k, v]) => serviceChip(k, v)).join('') +
        '</div></div>' +
      '<div class="card"><div class="label muted tiny" style="text-transform:uppercase;' +
        'font-size:11.5px;letter-spacing:.6px;font-weight:600">Resources</div>' +
        '<div class="kv" style="padding:10px 0;grid-template-columns:110px 1fr">' +
          '<div class="k">Uptime</div><div class="v" style="font-family:inherit">' +
            upStr + '</div>' +
          '<div class="k">Disk</div><div class="v" style="font-family:inherit">' +
            fmtBytes(disk.used) + ' / ' + fmtBytes(disk.total) + ' (' +
            (disk.percent || 0) + '%)</div>' +
          '<div class="k">Memory</div><div class="v" style="font-family:inherit">' +
            fmtBytes(mem.used) + ' / ' + fmtBytes(mem.total) + ' (' +
            (mem.percent || 0) + '%)</div>' +
          '<div class="k">Firmware blobs</div><div class="v" style="font-family:inherit">' +
            (st.blob_count || 0) + ' &middot; ' + fmtBytes(st.firmware_root_bytes) +
            '</div>' +
          '<div class="k">OTA log</div><div class="v" style="font-family:inherit">' +
            fmtBytes(st.log_bytes) + '</div>' +
        '</div></div>' +
      '<div class="card"><div class="label muted tiny" style="text-transform:uppercase;' +
        'font-size:11.5px;letter-spacing:.6px;font-weight:600">OTA self-test</div>' +
        '<div style="margin-top:12px">' +
        (ls.ok
          ? '<div class="banner ok" style="margin:0">:8080 answered &mdash; ' +
            esc(ls.detail) + '</div>'
          : '<div class="banner err" style="margin:0">:8080 failed &mdash; ' +
            esc(ls.detail || '') + '</div>') +
        '<div class="tiny muted" style="margin-top:12px">Console :8082 &middot; ' +
          'OTA API :8080 &middot; monitor :8081</div>' +
        '</div></div>' +
    '</div>' +
    '<h2>Credentials</h2>' +
    '<div class="card" style="max-width:520px">' +
      '<form id="pw-form">' +
        '<label class="f"><span>Current password</span>' +
          '<input type="password" id="pw-current" required></label>' +
        '<label class="f"><span>New password (min 8 characters)</span>' +
          '<input type="password" id="pw-new" required></label>' +
        '<button class="btn primary" type="submit">Update password</button>' +
      '</form></div>';
}

// ---------------------------------------------------------------- data load
async function loadOverview() {
  const d = await api('/api/overview');
  $('#hostname').textContent = d.health ? d.health.host : '';
  state.channels = d.channels || [];
  $('#view').innerHTML = renderOverview(d);
}

async function loadChannels() {
  const d = await api('/api/channels');
  state.channels = d.channels || [];
  $('#view').innerHTML = renderChannels();
}

async function loadFleet() {
  state.fleet = await api('/api/fleet');
  $('#view').innerHTML = renderFleet();
}

async function loadEvents() {
  const q = new URLSearchParams({ limit: '300' });
  if (state.filters.event) q.set('event', state.filters.event);
  if (state.filters.device) q.set('device', state.filters.device);
  if (state.filters.q) q.set('q', state.filters.q);
  const d = await api('/api/events?' + q.toString());
  const host = $('#events-table');
  if (host) host.innerHTML = renderEventsTable(d);
}

async function loadPublish() {
  if (!state.channels.length) {
    try { state.channels = (await api('/api/channels')).channels || []; } catch (e) {}
  }
  $('#view').innerHTML = renderPublish();
  bindPublish();
}

async function loadSystem() {
  const d = await api('/api/health');
  $('#view').innerHTML = renderSystem(d);
  bindPassword();
}

// ---------------------------------------------------------------- actions
async function ensureChannels() {
  if (state.channels && state.channels.length) return state.channels;
  try { state.channels = (await api('/api/channels')).channels || []; } catch (e) {}
  return state.channels;
}

function showLoadError(err) {
  $('#view').innerHTML = '<div class="banner err">Failed to load: ' +
    esc(err.message) + '</div>';
}

function refreshView() {
  if (state.timer) { clearInterval(state.timer); state.timer = null; }
  const v = state.view;

  if (v === 'overview') {
    loadOverview().catch(showLoadError);
    state.timer = setInterval(() => loadOverview().catch(() => {}), 20000);
    return;
  }
  if (v === 'channels') {
    loadChannels().catch(showLoadError);
    return;
  }
  if (v === 'fleet') {
    loadFleet().catch(showLoadError);
    state.timer = setInterval(() => loadFleet().catch(() => {}), 20000);
    return;
  }
  if (v === 'events') {
    // shell first (needs the channel list for its filter), then the table;
    // auto-refresh touches only the table so filter selections survive.
    $('#view').innerHTML = '<div class="empty"><span class="spin"></span></div>';
    ensureChannels()
      .then(() => { $('#view').innerHTML = renderEvents(); return loadEvents(); })
      .catch(showLoadError);
    state.timer = setInterval(() => loadEvents().catch(() => {}), 15000);
    return;
  }
  if (v === 'publish') {
    loadPublish().catch(showLoadError);
    return;
  }
  if (v === 'system') {
    loadSystem().catch(showLoadError);
    return;
  }
}

function setView(v) {
  state.view = v;
  $$('#nav button').forEach(b =>
    b.classList.toggle('active', b.dataset.view === v));
  location.hash = v;
  refreshView();
}

function bindPublish() {
  const form = $('#pub-form');
  if (!form) return;
  form.addEventListener('submit', async (e) => {
    e.preventDefault();
    const status = $('#pub-status');
    const submit = $('#pub-submit');
    const fileInput = $('#pub-file');
    const file = fileInput.files && fileInput.files[0];
    if (!file) { toast('Choose a firmware file first', 'err'); return; }

    const newChan = ($('#pub-newchan').value || '').trim().toLowerCase();
    const device = newChan || $('#pub-device').value;
    const version = ($('#pub-version').value || '').trim();
    if (!device || !version) { toast('Channel and version are required', 'err'); return; }

    const fd = new FormData();
    fd.append('device', device);
    fd.append('version', version);
    fd.append('note', ($('#pub-note').value || '').trim());
    fd.append('firmware', file, file.name);
    if ($('#pub-confirm').checked) fd.append('confirm', '1');

    submit.disabled = true;
    status.innerHTML = '<span class="spin"></span> uploading ' + fmtBytes(file.size) + '...';
    try {
      const res = await api('/api/publish', { body: fd });
      status.textContent = '';
      $('#pub-result').innerHTML =
        '<div class="banner ok"><div><b>Published v' + esc(res.manifest.latest_version) +
        '</b> to <span class="mono">' + esc(res.manifest.device_type) + '</span><br>' +
        'file <span class="mono">' + esc(res.manifest.filename) + '</span> &middot; ' +
        fmtBytes(res.manifest.size) + '<br>' +
        'sha256 <span class="mono">' + esc(res.manifest.sha256) + '</span>' +
        (res.renamed_from ? '<br><span class="tiny">renamed from ' +
          esc(res.renamed_from) + '</span>' : '') + '</div></div>';
      toast('Published ' + res.manifest.device_type + ' v' + res.manifest.latest_version, 'ok');
      form.reset();
      state.channels = (await api('/api/channels')).channels || [];
    } catch (err) {
      status.textContent = '';
      if (err.payload && err.payload.requires_confirmation) {
        $('#pub-result').innerHTML = '<div class="banner warn"><div>' +
          esc(err.message) + '<br>Tick the confirmation box above and publish ' +
          'again if you are certain.</div></div>';
      } else {
        $('#pub-result').innerHTML = '<div class="banner err">' + esc(err.message) +
          '</div>';
      }
      toast(err.message, 'err');
    } finally {
      submit.disabled = false;
    }
  });

  $('#pub-newchan').addEventListener('input', (e) => {
    const v = e.target.value.trim();
    $('#pub-device').disabled = !!v;
  });
}

function bindPassword() {
  const form = $('#pw-form');
  if (!form) return;
  form.addEventListener('submit', async (e) => {
    e.preventDefault();
    const fd = new URLSearchParams();
    fd.append('current', $('#pw-current').value);
    fd.append('new', $('#pw-new').value);
    try {
      await api('/api/password', { body: fd });
      toast('Password updated', 'ok');
      form.reset();
    } catch (err) {
      toast(err.message, 'err');
    }
  });
}

// delegated clicks
document.addEventListener('click', async (e) => {
  const nav = e.target.closest('#nav button');
  if (nav) { setView(nav.dataset.view); return; }

  const toggle = e.target.closest('[data-toggle]');
  if (toggle) {
    const name = toggle.dataset.toggle;
    if (state.openChannels.has(name)) state.openChannels.delete(name);
    else state.openChannels.add(name);
    const card = toggle.closest('.chan');
    if (card) card.classList.toggle('open');
    return;
  }

  const promote = e.target.closest('[data-promote]');
  if (promote) {
    const device = promote.dataset.promote;
    const file = promote.dataset.file;
    const ver = promote.dataset.ver;
    if (!confirm('Point channel ' + device + ' at v' + ver + ' (' + file + ')?\n\n' +
                 'Devices on this channel will pull that image on their next poll.')) return;
    try {
      const res = await api('/api/promote', { json: { device, file, version: ver } });
      toast('Channel ' + device + ' now serves v' + res.manifest.latest_version, 'ok');
      loadChannels();
    } catch (err) { toast(err.message, 'err'); }
    return;
  }

  const del = e.target.closest('[data-del]');
  if (del) {
    const device = del.dataset.del;
    const file = del.dataset.file;
    if (!confirm('Delete ' + file + ' from ' + device + '?\n\nThis cannot be undone.')) return;
    try {
      const res = await api('/api/delete', { json: { device, file } });
      toast('Deleted ' + file + ' (' + fmtBytes(res.freed_bytes) + ' freed)', 'ok');
      loadChannels();
    } catch (err) { toast(err.message, 'err'); }
    return;
  }

  const verify = e.target.closest('[data-verify]');
  if (verify) {
    const device = verify.dataset.verify;
    verify.disabled = true;
    try {
      const res = await api('/api/verify', { json: { device } });
      if (res.ok) toast('Checksum OK for ' + device + ' (' + res.filename + ')', 'ok');
      else toast('Verification FAILED for ' + device + ': ' + (res.reason || ''), 'err');
    } catch (err) { toast(err.message, 'err'); }
    finally { verify.disabled = false; }
    return;
  }

  const apply = e.target.closest('#f-apply');
  if (apply) {
    state.filters.event = $('#f-event').value;
    state.filters.device = $('#f-device').value;
    state.filters.q = $('#f-q').value;
    loadEvents().catch(err => toast(err.message, 'err'));
    return;
  }
});

// search on Enter
document.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && e.target.id === 'f-q') {
    const btn = $('#f-apply');
    if (btn) btn.click();
  }
});

// clock
setInterval(() => {
  const el = $('#clock');
  if (el) el.textContent = new Date().toLocaleTimeString();
}, 1000);

// boot
(function boot() {
  const fromHash = (location.hash || '').replace('#', '');
  const known = ['overview', 'channels', 'fleet', 'events', 'publish', 'system'];
  state.view = known.includes(fromHash) ? fromHash : 'overview';
  $$('#nav button').forEach(b =>
    b.classList.toggle('active', b.dataset.view === state.view));
  refreshView();
})();
"""


# --------------------------------------------------------------------------- #
# entry point
# --------------------------------------------------------------------------- #

def main() -> None:
    port = DEFAULT_PORT
    if "--init-only" in sys.argv:
        cfg = load_config(create=True)
        print(f"config at {CONFIG_PATH}, user {cfg.get('user')}")
        return
    for i, arg in enumerate(sys.argv):
        if arg == "--port" and i + 1 < len(sys.argv):
            port = int(sys.argv[i + 1])

    cfg = load_config(create=True)
    os.makedirs(ROOT, exist_ok=True)

    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    server.daemon_threads = True
    server.cfg = cfg          # type: ignore[attr-defined]
    print(f"OTA management console listening on :{port} "
          f"(firmware root {ROOT}, OTA API :{OTA_PORT})", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("stopping", flush=True)


if __name__ == "__main__":
    main()
