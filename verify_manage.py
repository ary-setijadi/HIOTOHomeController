#!/usr/bin/env python3
"""End-to-end verification of the OTA management console at :8082.

Exercises auth, every read API, the full publish -> verify -> promote -> delete
lifecycle on a scratch channel, and the security guards. Cleans up after itself.
"""
from __future__ import annotations

import json
import sys
import uuid
import urllib.error
import urllib.request

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

BASE = "http://192.168.1.99:8082"
USER = "admin"
PASSWORD = "Hioto-OTA-2026"
SCRATCH = "zz-scratch-test"

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  [{detail}]" if detail and not ok else ""))


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """Surface 3xx responses instead of following them (we assert on them)."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


OPENER = urllib.request.build_opener(NoRedirect)


class Client:
    def __init__(self):
        self.cookie = ""

    def req(self, path, method="GET", body=None, headers=None, ctype=None):
        url = BASE + path
        data = None
        hdrs = dict(headers or {})
        if body is not None:
            data = body if isinstance(body, bytes) else body.encode()
            if ctype:
                hdrs["Content-Type"] = ctype
        if self.cookie:
            hdrs["Cookie"] = self.cookie
        req = urllib.request.Request(url, data=data, headers=hdrs, method=method)
        try:
            with OPENER.open(req, timeout=20) as r:
                return r.status, r.read(), dict(r.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read(), dict(e.headers)

    def login(self, user=USER, password=PASSWORD):
        status, body, headers = self.req(
            "/login", "POST", f"user={user}&password={password}",
            ctype="application/x-www-form-urlencoded")
        sc = headers.get("Set-Cookie", "")
        if sc:
            self.cookie = sc.split(";")[0]
        return status

    def jget(self, path):
        s, b, _ = self.req(path)
        try:
            return s, json.loads(b)
        except Exception:
            return s, b


def multipart(fields, file_field, filename, file_bytes):
    boundary = "----ota" + uuid.uuid4().hex
    out = []
    for k, v in fields.items():
        out.append(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n".encode())
    out.append(
        f"--{boundary}\r\nContent-Disposition: form-data; name=\"{file_field}\"; "
        f"filename=\"{filename}\"\r\nContent-Type: application/octet-stream\r\n\r\n".encode())
    out.append(file_bytes)
    out.append(f"\r\n--{boundary}--\r\n".encode())
    return b"".join(out), f"multipart/form-data; boundary={boundary}"


def main():
    print("=== unauthenticated surface ===")
    anon = Client()
    s, b, _ = anon.req("/login")
    check("GET /login serves the login page", s == 200 and b"Sign in" in b, f"{s}")
    s, b, h = anon.req("/")
    check("GET / without session redirects to /login", s == 303 and "/login" in h.get("Location", ""), f"{s} {h.get('Location')}")
    for path in ("/api/overview", "/api/channels", "/api/fleet", "/api/events",
                 "/api/health", "/api/channel?type=esp32"):
        s, _b, _h = anon.req(path)
        check(f"{path} rejects anonymous access", s == 401, f"{s}")

    print("\n=== login ===")
    c = Client()
    s = c.login()
    check("login with correct password", s == 303 and c.cookie, f"{s} cookie={bool(c.cookie)}")
    bad = Client()
    check("login with wrong password is rejected", bad.login(password="definitely-wrong") == 401)

    print("\n=== read APIs ===")
    s, d = c.jget("/api/overview")
    check("overview returns channels", s == 200 and isinstance(d.get("channels"), list),
          f"{s}")
    chans = {ch["device_type"] for ch in d.get("channels", [])}
    check("overview lists the two production channels",
          {"hioto-genmon-28-st7789", "hioto-genmon-35-st7796"} <= chans, str(sorted(chans)))
    check("overview includes health + self-test",
          d.get("health", {}).get("ota_selftest", {}).get("ok") is True)
    check("overview fleet_counts present", "counts" in str(d.get("fleet_counts")) or isinstance(d.get("fleet_counts"), dict))

    s, d = c.jget("/api/channels")
    check("channels returns history", s == 200 and all("history" in c_ for c_ in d["channels"]),
          f"{s}")
    prod = [c_ for c_ in d["channels"] if c_["device_type"] == "hioto-genmon-28-st7789"]
    if prod:
        p = prod[0]
        check("production channel has a live blob on disk", p.get("live_present") is True)
        check("production channel history non-empty", p.get("version_count", 0) > 0,
              str(p.get("version_count")))

    s, d = c.jget("/api/fleet")
    check("fleet returns devices", s == 200 and len(d.get("devices", [])) > 0, f"{s}")
    real = [x for x in d["devices"] if not x.get("synthetic")]
    check("fleet recognises real ESP32 devices", len(real) > 0, str(len(real)))
    check("fleet device states are valid",
          all(x.get("state") in ("current", "outdated", "ahead", "unknown", "unreported", "probe")
              for x in d["devices"]))

    s, d = c.jget("/api/events?limit=5")
    check("events returns records", s == 200 and d.get("count", 0) > 0, f"{s}")
    s, d = c.jget("/api/events?event=report&limit=5")
    check("events filters by type", all(e.get("event") == "report" for e in d.get("events", [])))

    s, d = c.jget("/api/health")
    check("health reports services", s == 200 and "ota-firmware" in d.get("services", {}))
    check("health ota self-test ok", d.get("ota_selftest", {}).get("ok") is True,
          str(d.get("ota_selftest", {}).get("detail")))

    s, d = c.jget("/api/channel?type=hioto-genmon-28-st7789")
    check("channel detail returns hashed history", s == 200 and all("sha256" in h for h in d["history"]))
    live = [h for h in d["history"] if h["live"]]
    check("live blob hash matches manifest", bool(live) and live[0].get("sha256_matches_live") is True)

    s, b, _ = c.req("/app.css")
    check("app.css served", s == 200 and b"--bg" in b, f"{s}")
    s, b, _ = c.req("/app.js")
    check("app.js served", s == 200 and b"renderOverview" in b, f"{s}")

    print("\n=== security guards ===")
    s, b, _ = c.req("/api/promote", "POST", json.dumps({"device": "esp32", "file": "x.bin"}),
                    {"Content-Type": "application/json"})
    check("mutating call without X-OTA-MGR is refused", s == 403, f"{s}")
    s, b, _ = c.req("/api/delete", "POST",
                    json.dumps({"device": "../etc", "file": "passwd"}),
                    {"Content-Type": "application/json", "X-OTA-MGR": "1"})
    check("path traversal in device is refused", s == 400, f"{s}")
    s, b, _ = c.req("/api/delete", "POST",
                    json.dumps({"device": "esp32", "file": "../../etc/passwd"}),
                    {"Content-Type": "application/json", "X-OTA-MGR": "1"})
    check("path traversal in filename is refused", s == 400, f"{s}")
    s, b, _ = c.req("/api/download?type=esp32&file=../../etc/passwd")
    check("download traversal is refused", s == 400, f"{s}")
    s, b, _ = c.req("/api/delete", "POST",
                    json.dumps({"device": "hioto-genmon-28-st7789",
                                "file": "fw-hioto-genmon-28-st7789-2.0.6.bin"}),
                    {"Content-Type": "application/json", "X-OTA-MGR": "1"})
    check("refuses to delete the live blob", s == 400 and b"refusing" in b, f"{s}")

    print("\n=== publish lifecycle (scratch channel) ===")
    payload = b"OTA-CONSOLE-SELFTEST" + bytes(range(256)) * 8
    body, ctype = multipart(
        {"device": SCRATCH, "version": "0.0.1", "note": "console self-test"},
        "firmware", "selftest.bin", payload)
    s, b, _ = c.req("/api/publish", "POST", body,
                    {"X-OTA-MGR": "1", "Content-Type": ctype})
    check("publish accepts a new channel", s == 200, f"{s} {b[:160]}")
    published = json.loads(b) if s == 200 else {}

    s, d = c.jget(f"/api/channel?type={SCRATCH}")
    check("new channel is visible with history", s == 200 and len(d.get("history", [])) == 1, f"{s}")
    check("published file follows the house naming convention",
          d.get("manifest", {}).get("filename") == f"fw-{SCRATCH}-0.0.1.bin",
          str(d.get("manifest", {}).get("filename")))
    check("published size matches the upload",
          d.get("manifest", {}).get("size") == len(payload))

    s, d = c.jget(f"/api/channel?type={SCRATCH}")
    manifest_sha = d["manifest"]["sha256"]
    import hashlib
    check("published sha256 is the hash of the bytes",
          manifest_sha == hashlib.sha256(payload).hexdigest())

    # download round-trip through the console
    s, b, _ = c.req(f"/api/download?type={SCRATCH}&file=fw-{SCRATCH}-0.0.1.bin")
    check("console can serve the blob back byte-identically", s == 200 and b == payload, f"{s}")

    # the device-facing OTA server must see it too
    with urllib.request.urlopen(f"http://192.168.1.99:8080/api/latest?device={SCRATCH}&guid=verify", timeout=10) as r:
        m = json.load(r)
    check("device-facing OTA API serves the new manifest", m.get("latest_version") == "0.0.1",
          str(m.get("latest_version")))
    with urllib.request.urlopen(m["url"], timeout=10) as r:
        dl = r.read()
    check("devices can download the published image", dl == payload, f"{len(dl)} bytes")

    # verify / second release / promote / delete
    s, b, _ = c.req("/api/verify", "POST", json.dumps({"device": SCRATCH}),
                    {"Content-Type": "application/json", "X-OTA-MGR": "1"})
    v = json.loads(b)
    check("verify reports a matching checksum", s == 200 and v.get("ok") is True, str(v.get("reason")))

    body, ctype = multipart(
        {"device": SCRATCH, "version": "0.0.2", "note": "second build"},
        "firmware", "selftest2.bin", payload + b"-v2")
    s, b, _ = c.req("/api/publish", "POST", body, {"X-OTA-MGR": "1", "Content-Type": ctype})
    check("second publish succeeds", s == 200, f"{s}")
    s, d = c.jget(f"/api/channel?type={SCRATCH}")
    check("channel now has two blobs", len(d.get("history", [])) == 2, str(len(d.get("history", []))))

    s, b, _ = c.req("/api/promote", "POST",
                    json.dumps({"device": SCRATCH, "file": f"fw-{SCRATCH}-0.0.1.bin",
                                "version": "0.0.1"}),
                    {"Content-Type": "application/json", "X-OTA-MGR": "1"})
    ok = s == 200 and json.loads(b)["manifest"]["latest_version"] == "0.0.1"
    check("promote rolls the channel back", ok, f"{s} {b[:120]}")
    with urllib.request.urlopen(f"http://192.168.1.99:8080/api/latest?device={SCRATCH}&guid=verify2", timeout=10) as r:
        m2 = json.load(r)
    check("device-facing API reflects the rollback", m2.get("latest_version") == "0.0.1",
          str(m2.get("latest_version")))

    s, b, _ = c.req("/api/delete", "POST",
                    json.dumps({"device": SCRATCH, "file": f"fw-{SCRATCH}-0.0.2.bin"}),
                    {"Content-Type": "application/json", "X-OTA-MGR": "1"})
    check("delete removes a stale blob", s == 200, f"{s} {b[:120]}")

    s, b, _ = c.req("/api/note", "POST", json.dumps({"device": SCRATCH, "note": "noted"}),
                    {"Content-Type": "application/json", "X-OTA-MGR": "1"})
    check("note update works", s == 200 and json.loads(b)["manifest"]["note"] == "noted", f"{s}")

    print("\n=== board-safety guard ===")
    body, ctype = multipart(
        {"device": "hioto-genmon-28-st7789", "version": "9.9.9", "note": "wrong panel"},
        "firmware", "fw-hioto-genmon-35-st7796-9.9.9.bin", b"wrong-panel-image")
    s, b, _ = c.req("/api/publish", "POST", body, {"X-OTA-MGR": "1", "Content-Type": ctype})
    payload_err = json.loads(b) if b else {}
    check("cross-channel image is blocked without confirmation",
          s == 409 and payload_err.get("requires_confirmation") is True, f"{s} {b[:140]}")

    print("\n=== cleanup ===")
    # remove the scratch channel entirely
    import paramiko
    c2 = paramiko.SSHClient()
    c2.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c2.connect("192.168.1.99", username="orangepi", password="OrangePiUser1!",
               timeout=20, look_for_keys=False, allow_agent=False)
    _i, o, _e = c2.exec_command(
        f"echo 'OrangePiUser1!' | sudo -S rm -rf /srv/ota/{SCRATCH} 2>&1; "
        f"test -d /srv/ota/{SCRATCH} && echo STILL_THERE || echo REMOVED")
    out = o.read().decode("utf-8", "replace")
    c2.close()
    check("scratch channel removed from the server", "REMOVED" in out, out.strip()[-60:])

    s, d = c.jget("/api/channels")
    check("scratch channel gone from the console",
          SCRATCH not in {x["device_type"] for x in d["channels"]})
    with urllib.request.urlopen("http://192.168.1.99:8080/api/devices", timeout=10) as r:
        devs = json.load(r)["devices"]
    check("device-facing channel list is unchanged", SCRATCH not in devs, str(devs))

    print("\n" + "=" * 66)
    passed = sum(1 for _n, ok, _d in results if ok)
    print(f"RESULT: {passed}/{len(results)} checks passed")
    failed = [n for n, ok, _d in results if not ok]
    if failed:
        print("FAILED:")
        for n in failed:
            print("  -", n)
        sys.exit(1)
    print("ALL CHECKS PASSED")


if __name__ == "__main__":
    main()
