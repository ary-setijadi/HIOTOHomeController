# Orange Pi PC — Pi-hole + OTA Firmware Server

**Documented:** 2026-09-29
**Device:** Orange Pi PC (Allwinner H3, 1 GB RAM)
**Hostname:** `orangepipc`

---

## 1. Summary

| Service | Status | Address |
|---|---|---|
| Pi-hole (DNS + ad blocking) | ✅ running | `http://192.168.1.99/admin` |
| DNS server | ✅ port 53 | `192.168.1.99` |
| SSH | ✅ port 22 | `root@192.168.1.99` |
| OTA firmware server | ✅ port 8080 | `http://192.168.1.99:8080` |

The board runs **Armbian** (Debian 13 "trixie", kernel 6.18.52) on the microSD, serves as the network **Pi-hole DNS / ad blocker**, and hosts a **device-type-aware OTA firmware server** with per-device GUID tracking and full lifecycle logging.

---

## 2. Network

- **Static IP:** `192.168.1.99/24`
- **Gateway:** `192.168.1.1`
- **DNS (Pi resolver):** `192.168.1.1`
- **Pi-hole upstream DNS:** `192.168.1.1` (router)
- Network config managed by **netplan → systemd-networkd**, file `/etc/netplan/20-static-end0.yaml`, interface `end0`.

> Notes:
> - This PC manages the Pi from the Wi-Fi subnet `192.168.31.x`, routed to `192.168.1.x`.
> - **ICMP (ping) to the Pi is filtered** by the router, but SSH/DNS/HTTP all work.
> - The router enforces **Google SafeSearch** (`google.com` → `forcesafesearch.google.com`).

---

## 3. Credentials

| Item | Value |
|---|---|
| SSH root | `root` / `OrangePiRoot1!` |
| SSH user (sudo) | `orangepi` / `OrangePiUser1!` |
| Pi-hole web admin | `OrangePiWeb2026` |

> These are functional credentials set during setup. Change them if this box will be internet-facing.

---

## 4. Pi-hole (v6)

- Core `6.4.3`, Web `6.6`, FTL `6.7.1`.
- **74,761 domains** blocked (StevenBlack list), blocking enabled.
- Web UI: `http://192.168.1.99/admin` (HTTP 80 + HTTPS 443 self-signed).

### Local DNS record
`MainController.hioto` → `192.168.1.22`

Managed in the web UI → **Local DNS → DNS Records**, or via:
```bash
pihole-FTL --config dns.hosts '["192.168.1.22 MainController.hioto"]'
```

---

## 5. OTA Firmware Server (v4)

Runs as systemd service **`ota-firmware`** on port **8080**, auto-starts on boot.

### Storage layout (on the Pi)
```
/srv/ota/
├── <device_type>/
│   ├── firmware/          # firmware blobs
│   └── latest.json        # manifest for this device type
└── ota.log                # JSONL event log
```

### API reference

| Method | Endpoint | Description | Logged |
|---|---|---|---|
| GET | `/api/devices` | list device types | — |
| GET | `/api/latest?device=<type>&guid=<id>` | latest manifest | `query` |
| GET | `/api/<type>/latest?guid=<id>` | latest manifest (path form) | `query` |
| GET | `/firmware/<type>/<file>?guid=<id>` | download firmware | `download` (`ota_execution`) |
| POST | `/api/report` | device reports post-flash result | `report` (`ota_report`) |
| GET | `/api/log?limit=100` | view recent events | — |

**Device GUID** may be passed as a query param `?guid=<id>` or header `X-Device-GUID`.

### `GET /api/latest?device=esp32&guid=...` response
```json
{
  "device_type": "esp32",
  "latest_version": "1.0.0",
  "filename": "fw-esp32-1.0.0.bin",
  "url": "http://192.168.1.99:8080/firmware/esp32/fw-esp32-1.0.0.bin",
  "size": 21,
  "sha256": "770413917c8dee0f218c27e5ef4f530e9d5d915f88d5b16a621b6d9b9e137097",
  "released_at": "2026-09-29T23:24:41.26Z",
  "note": "esp32 test build"
}
```

### `POST /api/report` body
```json
{
  "device_type": "esp32",
  "guid": "device-1111-aaaa",
  "status": "success",        // or "failed"
  "from_version": "0.9.0",
  "to_version": "1.0.0",
  "message": "flash ok"        // optional
}
```
Response: `{"ok":true,"logged":{...}}`

### Log event schema (JSONL)
```json
{"time":"2026-09-29T23:26:18Z","ts":1790724378.49,
 "event":"download",                    // query | download | report
 "device_type":"esp32","guid":"device-1111-aaaa",
 "ip":"192.168.1.250","user_agent":"curl/8.21.0",
 "filename":"fw-esp32-1.0.0.bin","size":21,
 "action":"ota_execution"}
```

### Publish new firmware
```bash
# on the Pi
python3 /usr/local/bin/publish_firmware.py <file> --device <type> --version X.Y.Z --note "changelog"
```
Computes size + SHA-256 and updates `latest.json` for that device type.

### Device integration flow
1. `GET /api/latest?device=<type>&guid=<guid>` → compare `latest_version` to running version.
2. If newer → `GET /firmware/<type>/<file>?guid=<guid>` → flash → verify `sha256`.
3. `POST /api/report` with `status`, `from_version`, `to_version`, optional `message`.

---

## 6. Workspace artifacts

Created during this session in `C:\Users\T495\Documents\MEGA\_OrangePiBasic\`:

| File | Purpose |
|---|---|
| `flash_image.ps1`, `reflash_verify.ps1` | raw flash + full-hash verification of the SD card |
| `find_pi.ps1` | ping-sweep `192.168.137.x` to locate the board |
| `ssh_bootstrap.py`, `ssh_cmd.py` | SSH provisioning + one-shot command runner (paramiko) |
| `set_static_ip.py`, `fix_dns.py`, `set_ip_99.py` | netplan static-IP + DNS configuration |
| `pihole_configure.py`, `add_local_dns.py` | Pi-hole config + local DNS record |
| `ota_server.py` | OTA HTTP server (deployed to `/usr/local/bin/ota_server.py`) |
| `publish_firmware.py` | firmware publish tool (deployed to `/usr/local/bin/publish_firmware.py`) |
| `ota-firmware.service` | systemd unit (deployed to `/etc/systemd/system/`) |
| `deploy_ota.py`, `update_ota.py` | deploy/update scripts |
| `audit.py` | security posture audit |
| `SETUP-OrangePi-PiHole-OTA.md` | this document |

---

## 7. Key lessons / troubleshooting notes

1. **Flaky SD-card reader** silently corrupted the first flash (only full SHA-256 read-back caught it). A PC reboot fixed the reader; always full-verify after writing.
2. **Counterfeit microSD** — the "64 GB" card actually reported 16 GB and behaved erratically. Used the known-good 16 GB card instead.
3. **External DNS blocked** — `1.1.1.1` was unreachable from the Pi; use the gateway DNS (`192.168.1.1` / `192.168.31.1`).
4. **Power** — the Orange Pi PC needs a solid 5 V / 2 A; thin jumper-wire GPIO power caused brown-out boot failures.
5. **Pi-hole v6 unattended install** requires a pre-created `/etc/pihole/pihole.toml` (skips the TTY dialogs), then `--unattended`.
6. **Environment is PowerShell 5.1** — no `ForEach-Object -Parallel`, and native-command quoting is fragile (use `Invoke-RestMethod` / Python for JSON).
7. **SSH over TCP works even where ICMP is filtered** — don't rely on ping to judge reachability.

---

## 8. Still open (if desired later)

- **Hardening** (paused): disable root SSH, add `ufw` firewall, stronger passwords, DNSSEC, unattended-upgrades.
- **Primary-DNS rollout**: set the router's DHCP DNS to `192.168.1.99` so all devices use Pi-hole.
- **OTA enhancements**: per-device rollout targeting, API key on `/api/report`, version history endpoint.
