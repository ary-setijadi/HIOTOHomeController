# OTA Management Console — `ota-manage`

A web console for the ESP32 OTA estate on `192.168.1.99` (Orange Pi PC,
`HiotoDNSServer`). It gives you one place to see firmware channels, publish and
roll back images, watch the device fleet, and read the request log.

```
http://192.168.1.99:8082/        user: admin    password: Hioto-OTA-2026
```

> Change the password under **System → Credentials** after the first login.
> `deploy_manage.py --reset-password` re-issues one if it is ever lost.

---

## 1. The environment as found

Audited on 2026-11-15 before anything was built:

| | |
|---|---|
| Host | Orange Pi PC (`sun8i`), armv7l, Armbian 26.11.0-trunk.52 (Debian 13 trixie) |
| Hostname / IP | `HiotoDNSServer` / `192.168.1.99` |
| Resources | 991 MB RAM, 15 GB eMMC (13 GB free), Python 3.13.5 |
| No web framework | no Flask/FastAPI/Django — everything is stdlib `http.server` |

Listening services:

| Port | Service | Role |
|---|---|---|
| 22 | `ssh` | admin access (`root` login is disabled; use `orangepi` + sudo) |
| 53 / 80 / 443 | `pihole-FTL` | Pi-hole DNS + admin UI |
| 8080 | `ota-firmware.service` | **device-facing OTA API** — ESP32 boards poll this |
| 8081 | `monitor-server.service` | `HDSMonitor` system/DNS dashboard |
| **8082** | `ota-manage.service` | **this console** (added) |

The two existing services were left completely untouched: `ota-firmware` was not
restarted and kept its original PID/uptime across the whole deployment.

### Channels found in `/srv/ota`

| Channel | Latest | Blobs | Status |
|---|---|---|---|
| `hioto-genmon-28-st7789` | 2.0.6 | 7 | **production** — 2.8" ESP32-2432S028, ST7789 |
| `hioto-genmon-35-st7796` | 2.0.6 | 7 | **production** — 3.5" ESP32-3248S035, ST7796 |
| `hioto-genmon-28` | 2.0.0 | 24 | retired (pre chipset-split name) |
| `hioto-genmon-35` | 2.0.0 | 11 | retired (pre chipset-split name) |
| `hioto-genmon` | 1.2.4 | 6 | retired (legacy shared name) |
| `esp32` | 1.0.0 | 1 | unrelated placeholder |
| `controller` | 2.3.1 | 1 | unrelated placeholder |

Log state at audit time: **6 762 events** — 6 675 polls, 43 downloads, 44
post-flash reports, 0 failures. Six real boards active
(`hioto-genmon-3c4b08`, `-3c3374`, `-3c4b98`, `-3d42cc`, `-70b134`, `-ec0940`)
across `192.168.1.125`, `.201`, `.250`, all reported `flash ok` on 2.0.6.

**Observations worth acting on**

1. **65 stale blobs** are retained across channels (~29 MB). The console lets you
   delete history you no longer need, and refuses to delete the live blob.
2. The three retired channels still answer `/api/latest`. Any board still
   compiled against an old channel name would keep pulling a stale image. Check
   the Fleet page — every board should be on `-28-st7789` or `-35-st7796`.
3. `ota.log` grows unbounded (1.6 MB so far). Nothing rotates it yet; see
   *Maintenance* below.

---

## 2. What the console does

| Page | Purpose |
|---|---|
| **Overview** | Health banner, live channels with version/size/SHA-256 and whether the served blob actually exists, fleet counters, 24 h poll/download/report counts, host resources, recent activity |
| **Channels** | Per-channel card: manifest details, pull URL, note, and full version history. Actions: **promote** (roll back/forward to any existing blob), **verify** (recompute SHA-256 and compare to `latest.json`), **delete** (history only) |
| **Fleet** | Every device GUID seen in the log: channel, running version, channel latest, up-to-date/outdated state, IP, last seen, online flag, poll/download/report counts |
| **Events** | The OTA log with filters by event type, channel and free text; auto-refreshes every 15 s |
| **Publish** | Upload a `.bin` with a version and note. Stored as `fw-<channel>-<version>.bin`, SHA-256 computed, `latest.json` rewritten atomically |
| **System** | Service states, uptime, disk, memory, blob count, live `:8080` self-test, password change |

### Safety behaviours

- **Panel guard.** Publishing an image whose filename names a *different* known
  channel returns `409` and requires explicit confirmation. A 2.8" ST7789 image
  on a 3.5" ST7796 board leaves a healthy board with a blank screen
  (`CHANNELS.md` → `!!! FIRMWARE TIDAK COCOK`).
- **Live-blob protection.** Delete refuses the file `latest.json` currently
  points at; demote it first.
- **Atomic manifests.** `latest.json` is written to a temp file in the same
  directory then `os.replace()`d, so a device polling mid-publish never sees a
  half-written manifest.
- **Integrity.** `verify` re-hashes what is actually on disk against the
  manifest, which catches bit-rot and interrupted uploads.

---

## 3. Layout on the Pi

```
/usr/local/bin/ota_manage.py            the console (single file, stdlib only)
/etc/systemd/system/ota-manage.service  systemd unit (root, Restart=always)
/etc/ota-manage.json                    credentials + session secret (0600)
/srv/ota/<channel>/latest.json          manifests (shared with ota_server.py)
/srv/ota/<channel>/firmware/*.bin       images
```

The console only ever reads/writes `/srv/ota`; it never calls into
`ota_server.py`, so the device path has no new dependency.

## 4. Operating it

```powershell
# install or upgrade (keeps the existing password)
python deploy_manage.py

# install with a password you choose, or issue a fresh one
python deploy_manage.py --password 'something-long'
python deploy_manage.py --reset-password
```

On the Pi:

```bash
sudo systemctl status ota-manage
sudo journalctl -u ota-manage -f
sudo systemctl restart ota-manage
```

## 5. HTTP API

All `/api/*` routes need a session cookie; mutating routes additionally require
the header `X-OTA-MGR: 1` (blocks cross-site form posts).

| Method | Route | Purpose |
|---|---|---|
| `GET` | `/api/overview` | dashboard aggregate |
| `GET` | `/api/channels` | channels + history |
| `GET` | `/api/channel?type=<ch>` | one channel with SHA-256 per blob |
| `GET` | `/api/fleet` | per-GUID device state |
| `GET` | `/api/events?limit&event&device&q` | filtered log |
| `GET` | `/api/health` | services, disk, memory, `:8080` self-test |
| `GET` | `/api/download?type=<ch>&file=<f>` | fetch a blob |
| `POST` | `/api/publish` | multipart: `device`, `version`, `note`, `firmware`, `confirm` |
| `POST` | `/api/promote` | JSON `{device, file, version?}` |
| `POST` | `/api/verify` | JSON `{device}` |
| `POST` | `/api/delete` | JSON `{device, file}` |
| `POST` | `/api/note` | JSON `{device, note}` |
| `POST` | `/api/password` | form: `current`, `new` |

Security model: PBKDF2-SHA256 (200 000 iterations) password, HMAC-signed
session cookie (`HttpOnly`, `SameSite=Strict`, 12 h), CSP with no inline script,
login throttling (8 failures / 5 min / IP), and path-traversal rejection on
every channel and filename.

## 6. Verification performed

- `verify_manage.py` — **52/52 checks**: anonymous access refused on every API,
  bad password rejected, every read endpoint, path-traversal guards, live-blob
  deletion refused, and the full publish → device-download → verify → promote →
  delete → note lifecycle against the live `:8080`, plus the panel guard.
- `ui_harness.js` — renders every view in a stubbed DOM against captured live
  API fixtures: no runtime exceptions, no `undefined`/`NaN` leakage, balanced
  markup, correct events-view wiring, HTML escaping of hostile field values.
- Screenshot pass (`make_preview.py` + `check_shot.py`) over all six views and
  the login page: dark theme painted, text present, accents correct.

To re-run the two harnesses later, regenerate their inputs first — both are dev
tools that read a snapshot of what the server serves:

```powershell
python make_fixtures.py     # -> _fixtures.json  (live API responses)
# then fetch https://192.168.1.99:8082/app.js to _served_app.js
node ui_harness.js
python make_preview.py overview   # -> _preview_overview.html + js
python check_shot.py _shot_overview.png
```

- Regression check: `ota-firmware` never restarted, all 7 channels still
  served, downloaded image SHA-256 matches its manifest, real boards kept
  polling during the whole deployment.

## 7. Maintenance ideas

- `ota.log` has no rotation. A `logrotate` rule for `/srv/ota/ota.log`
  (weekly, keep 8, `copytruncate`) would cap it — not applied automatically.
- 65 historical blobs can be culled from the Channels page; keeping one or two
  rollback targets per production channel is usually enough.
- Retired channels (`hioto-genmon`, `-28`, `-35`) could eventually be removed
  once no board is seen polling them.
