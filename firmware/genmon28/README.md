# HIOTO General Monitor — lamp switch panel (v1.0)

Firmware for the HIOTO CYD switch panels: **one code base, two builds**, one per
panel. Each build carries its own device type, so a board can only ever pull an
OTA image built for its own glass.

| Env | Board | Panel | Buttons |
|---|---|---|---|
| `genmon_35_st7796` | Sunton ESP32-3248S035 | ST7796, 320×480 → 480×320 | **8** (4 × 2) |
| `genmon_28_st7789` | Sunton ESP32-2432S028 | ST7789, 240×320 → 320×240 | **6** (3 × 2) |

Both are ESP32-D0WD-V3 with a CH340 USB-UART. The 3.5" panel wires the XPT2046
onto the display SPI bus (`TOUCH_CS=33`); the 2.8" panel gives it its own bus
(`TOUCH_SEPARATE_BUS`: clk 25, mosi 32, miso 39, cs 33, irq 36).

The button count is a single build-time constant, `GEN_SLOTS`, defined per
PlatformIO env and consumed by `config.h`. Everything else derives from it —
grid columns (`GEN_COLS = GEN_SLOTS / GEN_ROWS`), the Settings slot rows, and
every `slots[]` array — so the main screen and the configuration screen can
never disagree about how many buttons a board has. `DEFAULT_SLOT_LABELS` is
selected by `#if GEN_SLOTS`, and a build whose panel width does not match
`GEN_COLS` logs a `[ui] WARNING` at boot instead of silently re-flowing.

A **simple lamp switch panel**:

- Title bar **`[Floor] | [Room]`** (editable in Settings).
- **A grid of big buttons** (`GEN_SLOTS`, 4 × 2 on the 3.5" build, 3 × 2 on the
  2.8"), each with a lamp icon, the lamp's short id
  (e.g. `LAMP.14`) and its live ON/OFF state. Tap = toggle the lamp.
- **`Setelan` pill** opens Settings:
  - **WiFi**: **a scan list of the APs the radio hears** (tap one → the SSID is
    filled in, a secured network opens the password keyboard, an open one just
    needs *Simpan*). *Pindai AP* re-scans; the SSID/password fields below the
    list remain for a hidden SSID. Applied on save.
  - **Tampilan**: Floor and Room title fields.
  - **Layar & tema**: backlight dimmer (`−` / `+` / slider, 20–100 %) and the
    colour theme dropdown (applied live).
  - **Tombol lampu**: a **filter** (`Ruang ini` / `Lantai ini` / `Semua device`,
    with a `n dari m` counter), then per-button **device dropdown** (short ids
    such as `LAMP.14`) and **icon dropdown** (Spot / Umum / TL / Downlight /
    Taman).
  - **Simpan** persists everything to NVS and applies it.

Default assignment (first boot, before anything is stored in NVS):
`LAMP.14, LAMP.15, LAMP.09, LAMP.10, LAMP.11, LAMP.12` on the 6-button build,
plus `LAMP.13, LAMP.17` on the 8-button build.

The filter derives floor and room from each registry name (`LAMP.09-1ST.FLOOR-
LIVING ROOM-250-01` → floor `1ST.FLOOR`, room `LIVING ROOM`). The panel's Floor
and Room labels drive it: the first number of the floor label is compared
(`Lantai 1` → `1`), and the room labels are compared case/punctuation-blind
with a 5-character prefix tolerance so `CONNECTION SPACE` still matches
`CONNECTING SPACE`. Devices already used by a button are always appended, so a
filter can never hide an assigned lamp; if nothing matches, everything is
listed instead of an empty picker. With the defaults `Lantai 1` / `Living Room`
the room scope shows 6 lamps, the floor scope 38, and `Semua device` all 81.

## Colour themes

`src/theme.cpp` holds six complete palettes; a stored index selects one and the
UI is rebuilt from it (`ui_poll()`), so no widget logic depends on a theme:

| # | name | character |
|---|---|---|
| 0 | `Midnight` | deep navy, amber lamps |
| 1 | `Grafit` | neutral graphite, cyan accent, warm lamps |
| 2 | `Samudra` | deep teal, cool cyan lamps |
| 3 | `Senja` | warm plum dusk, orange lamps |
| 4 | `Terang` | light paper theme for sunlit rooms |
| 5 | `Neon` | black light, magenta lamps |

A theme carries background, card, border, text/muted/dim, accent/ok/warn/bad,
the lamp ON card + border + label colour, the icon ON/OFF/unknown fill and
outline, and the glow colour. Append new themes at the end — the stored index
is positional.

## Lamp icons

`ICON_NAMES` in `include/config.h` (index = stored value): `Spot` (head +
expanding beam), `Umum` (classic bulb + base), `TL` (tube + end caps),
`Downlight` (ceiling plate + dome + rays), `Taman` (post lamp: head + pole +
foot). Each style paints its body/border from the theme and shows a hot glow
spot only while the lamp is ON.

## Backlight dimmer

`src/backlight.cpp` drives `PIN_LCD_BL` (GPIO27) with 25 kHz PWM on LEDC channel
7 and a squared (gamma-ish) curve. Range is clamped to 5–100 % in the module and
20–100 % in the UI. Changes made with the slider are applied live and saved when
the knob is released; `−`/`+` save immediately.

## Lamp registry (controller CSV)

`src/registry.cpp` pulls `http://<controller>:8081/api/devices/export.csv` and
keeps every `AKTUATOR` row (currently **81**: 46 `LAMP.*` lamps, 21 smart plugs,
12 `LSKK` relays, 2 test relays). Capacity is `REG_MAX_DEVS 128`.

Each device carries two names:

- **compact label** for the panel button — `LAMP.12`, `SMARTSTEKER-16`,
  `DO-270-01`, `IWK-SAKLAR-79` (vendor prefixes `HIOTO`/`LSKK`/`lamp` are
  dropped, over-long joins fall back to first+last part, collisions get
  `#2`, `#3`).
- **full registry name** for the settings dropdown — shown verbatim so every
  entry is distinguishable (`LAMP.14-1ST.CONNECTING SPACE-252-02`).

The list is sorted by compact label, and the dropdown options are built in a
heap buffer sized from the real name lengths, so no entry can be truncated.
ON/OFF state learned over MQTT survives the 60 s refresh (devices are matched
by guid; the CSV `status` column is only a fallback). A saved slot resolves by
guid first, falls back to the compact label (`registry_resolve()`) and then
repairs the stored guid.

Inspect the whole list at runtime with `lamps` on the serial console.

## Lamp control (HIOTO protocol)

- Tap a button → publishes `Aktuator` `<guid>#0|1` — **active-low: 0 = ON, 1 = OFF**.
- State feedback: the panel subscribes `Aktuator` and `Sensor`, and also refreshes
  the controller registry every 60 s (the CSV export carries each lamp's last
  `status`, `0`/`1`), so lamps show their real state even when driven by a
  physical switch.

## MQTT

| topic | direction | payload |
|---|---|---|
| `iot/{id}/status` | device → broker, **retained** | `{"device":"…","status":"online"}` / LWT `"offline"` |
| `iot/{id}/telemetry` | device → broker, every 15 s | `{"device":"…","role":"switch-panel","floor":"…","room":"…","ip":"…"}` |
| `Status` | device → broker, every 30 s | `<id>#1` heartbeat |
| `Aktuator` | device → broker (toggle) / broker → device (state) | `<guid>#0|1` |
| `Sensor` | broker → device | `<guid>#0|1` switch state |

Device id: `hioto-genmon-XXXXXX` (derived from the MAC).

## OTA over Wi-Fi (Orange Pi OTA server)

The firmware pulls updates from the `ota-firmware` service on the Orange Pi PC
(`http://192.168.1.99:8080`), so no cable or BOOT button is needed after the
first flash:

1. `GET /api/latest?device=hioto-genmon&guid=<device id>` → manifest
2. if `latest_version` differs from `FW_VERSION`, stream the `url` into the OTA
   partition, then reboot
3. `POST /api/report` with `status`, `from_version`, `to_version`

Automatic checks run every 15 minutes (`OTA_CHECK_MS`) and can be disabled with
`otaauto 0`. Triggers:

- serial `ota` — check now
- serial `otaurl <url>` — flash an explicit blob
- serial `ver` — show version + last OTA status
- MQTT `iot/<device id>/cmd` with `{"ota":1}` — check now, no cable at all

Publishing a new build from the workstation:

```powershell
python ota_release.py --board 35 --publish
python ota_release.py --board 35 --no-build --publish   # republish what is already built
```

`FW_VERSION`, `FW_DEVICE_TYPE` and `OTA_BASE` live in `include/config.h`.

## Watchdog & self-healing (3.5.2)

A panel on a wall is out of reach, so two layers make it recover without a power
cycle. Both are logged, because the only way to notice a recovery is the serial
line - or the fact that the panel is still polling.

**Task watchdog** (`src/watchdog.cpp`): the ESP32 TWDT watches the Arduino loop
task with `WDT_TIMEOUT_S` = **30 s** and panics on a miss, which reboots the
board. It is fed from `loop()` every iteration and from every long-running loop
that does not return to it: the touch-calibration tap waits, the `disptest`
self test, `tptest`, and the OTA download write-loop. `[boot] reset=<reason>`
now reports *why* the last boot happened - `power-on`, `software (ESP.restart)`,
`panic`, `task watchdog`, `brownout` - so a self-reboot is distinguishable from a
mains power cycle.

**Wi-Fi / MQTT supervision** (`main.cpp`, `net_supervisor_poll()`):

- Disconnect **reason codes** are recorded from `ARDUINO_EVENT_WIFI_STA_DISCONNECTED`
  and printed (`[net] wifi LOST reason=15 (4way handshake timeout (wrong PSK / WPA3-only AP?))`).
  `WiFi.status()` alone only ever says "disconnected"; the reason tells a wrong
  password apart from an AP that vanished.
- While down: `WiFi.reconnect()` every 30 s, a full re-init (radio off/on) after
  3 min, and a **self-reboot after 15 min** - but only once the board has been
  online since power-on, so an absent AP or an unprovisioned board cannot turn
  into a reboot loop.
- After a recovery: MQTT is force-reconnected (the old socket is stale) and the
  ArduinoOTA listener is rebound to the new lease.
- MQTT: if it stays down while Wi-Fi is healthy, the laptop-style "turn it off
  and on" is applied to the radio (once per 2 min) and, as a last resort, the
  board reboots (30 min). Broker **credential rejections** (CONNACK 4/5) are
  reported and deliberately *not* retried with a reboot - that is a
  configuration error, not a hang.

`status` prints the live picture:

```
net: wifi=WiFi OK reason=0 mqtt=MQTT OK rc=0
wdt: aktif timeout=30s last-reset=power-on
```

Knobs live in `include/config.h`: `WDT_TIMEOUT_S`, `WIFI_RETRY_MS`,
`WIFI_REINIT_MS`, `WIFI_REBOOT_MS`, `MQTT_STALL_CYCLE_MS`,
`MQTT_STALL_REBOOT_MS`.

## Settings persistence (NVS namespace `hioto`)

`ssid`/`pass`, `mhost`/`mport`/`muser`/`mpass`, `devid`, `floor`, `room`,
`fid`/`rid` (controller floor/room ids), `bright` (backlight %), `theme`
(palette index), `scope` (picker filter), `oauto` (automatic OTA), `tcal` (touch
calibration), and per-slot `s{i}g` (guid), `s{i}l` (short label), `s{i}a`
(friendly alias), `s{i}i` (icon index).

## Serial console (COM3 @ 115200)

`help` · `status` · `wifi <ssid> <pass>` · `mqtt <host> <port> <user> <pass>` ·
`floor <label...>` · `room <label...>` · `bright [20-100]` · `theme [0-5]` ·
`lamps` · `scope [0-2]` · `cat` · `devid <name>` · `alias <n> <text>|` ·
`ver` · `ota` · `otaurl <url>` · `otaauto <0|1>` · `uigeom` ·
`ldr` · `cal` / `clearcal` / `traw` · `save` · `reboot`

## Flashing (manual download mode)

The CH340 auto-reset cannot enter download mode. Use the helper:

```powershell
cd firmware/hioto_general_monitor
python -m platformio run -e hioto_genmon
powershell -ExecutionPolicy Bypass -File tools\flash.ps1
```

When prompted: hold **BOOT**, tap **RESET** (or unplug + hold BOOT + replug),
keep BOOT ~1 s, release. The helper then flashes with `--before no_reset`.

## Notes

- BOOT short press opens Settings; holding BOOT ≥ 4 s factory-resets the NVS
  settings.
- Touch calibration: *Kalibrasi sentuh* in Settings, or the serial `cal` command
  (4 corner squares + centre validation, saved to NVS).
- Panel geometry: the ST7796 is 320×480 native; `TFT_WIDTH=320`,
  `TFT_HEIGHT=480` with `setRotation(1)` gives the 480×320 landscape UI.

## 2.8" CYD (ESP32-2432S028) — measured configuration

Verified against the unit with `firmware/cyd_probe` (see
`CYD-2432S028-CONTROL-REFERENCE.md`, Appendix B):

| Item | Value |
|---|---|
| Panel driver | **ST7789** (`-DST7789_DRIVER=1`) — ILI9341 renders black/snow |
| Colour order | **`TFT_RGB`** (BGR swaps red/blue) |
| Native size | 240x320 portrait ? `setRotation(1)` = 320x240 landscape |
| SPI host | HSPI (`-DUSE_HSPI_PORT`), 40 MHz |
| Backlight | GPIO 21, active HIGH |
| Touch controller | XPT2046 on its **own** bus: CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 |

Because the touch chip is not on the display bus, `TOUCH_CS` is unusable there;
`src/touch.cpp` drives it directly (`-DTOUCH_SEPARATE_BUS=1`) and keeps the
3.5" board on the TFT_eSPI touch extension. Calibration is a 4-point routine
(`cal` on the console or *Kalibrasi sentuh* in Settings) that stores raw
min/max plus swap/invert flags in NVS.

Verification status across the units brought up so far: display and UI are good
on every unit, and **touch is confirmed working**. The earlier "X=0 / Y~4070 /
IRQ stuck LOW" reading was a truncated SPI reply — the driver clocked one byte
after the command instead of two — fixed in `src/touch.cpp` by clocking
`cmd + hi + lo` and taking `(hi<<8|lo) >> 3`. Both calibrated units report
`swap=1` (the XPT2046 axes are rotated relative to the screen) and pass the
centre check within 3–4 px of a 16 px tolerance.

## Versions

**Two independent series, one per panel.** The 2.8" and 3.5" boards are different
hardware (controller, touch topology, button count) served by separate OTA
channels, so they carry separate version series and separate release cadence.
`FW_VERSION` is supplied per PlatformIO env, never shared; bumping one series
must never bump the other. Each build also asserts its own series at boot
(`FW_SERIES`, derived from `GEN_SLOTS`) and prints a loud warning if a 2.8" image
ever carries a 3.5.x version or vice versa.

| Series | Device type / env | Panel | Buttons |
|---|---|---|---|
| **3.5.x** | `hioto-genmon-35-st7796` / `genmon_35_st7796` | ST7796 480×320 | 8 (4 × 2) |
| **2.8.x** | `hioto-genmon-28-st7789` / `genmon_28_st7789` | ST7789 320×240 | 6 (3 × 2) |

Shared lineage before the split:

| Version | Notes |
|---|---|
| 1.3.0 | first 2.8" build: ST7789, separate touch bus, per-panel `FW_DEVICE_TYPE` |
| 1.3.1-1.3.5 | calibration logging, `tp` diagnostic, XPT2046 read corrected to 3 transfers |
| 1.3.6-1.3.8 | bit-banged touch experiments |
| 2.0.6 | last shared-version build, published for both device types |
| **3.5.1 / 2.8.1** | series split: `FW_VERSION` per env, `GEN_SLOTS` as the single source of truth, 6-button 2.8" layout |
| 2.8.2 | 2.8" only: touch presence taken from the pressure axis instead of PENIRQ polarity, so a board whose PENIRQ idles LOW (one delivered unit does) no longer inverts every tap. Confined to `#if TOUCH_SEPARATE_BUS`; the 3.5" branch is unchanged |
| **3.5.2** | overall task watchdog (30 s, panic → reboot) plus Wi-Fi/MQTT supervision — disconnect reason logging, reconnect/escalating re-init, self-reboot after 15 min Wi-Fi (or 30 min MQTT) loss, reset-reason logging. *These live in shared sources (`watchdog.cpp`, `main.cpp` supervisors), so the 2.8" build has shipped them too since 2.8.2 — only the 3.5 channel has published them.* See `docs/ota-channels.md` for the observed 28.6 h outage in the OTA log |
| **3.5.3** | Wi-Fi settings no longer require typing an SSID — Settings scans and shows a **listview of available APs** (strongest first, one row per SSID, `(terbuka)` marked), tapping a secured network opens the password keyboard, settings auto-rescan on open; manual fields stay for hidden networks. *Also shared source (`ui.cpp`), so present in the 2.8" build as well* |
| **2.8.3** | **OTA refuses downgrades.** The manifest check compared versions with a plain `strcmp`, so *any* difference counted as an update. The 2.8 channel still published `2.0.6`, so boards on `2.8.x` re-flashed themselves back to that old build and lost the watchdog and the AP picker — presenting exactly as "sometimes it cannot reconnect to Wi-Fi/MQTT" and "there is no network list". Versions are now compared numerically and an image is installed only when it is **newer** than the running one |