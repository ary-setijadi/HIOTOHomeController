# HIOTO General Monitor — lamp switch panel (v1.0)

Firmware for the **Sunton ESP32-3248S035** 3.5" CYD (ESP32-D0WD-V3, ST7796
320×480 native panel driven as 480×320 landscape, XPT2046 touch, CH340 @ COM3).

A **simple lamp switch panel**:

- Title bar **`[Floor] | [Room]`** (editable in Settings).
- **2 × 4 big buttons**, each with a lamp icon, the lamp's short id
  (e.g. `LAMP.14`) and its live ON/OFF state. Tap = toggle the lamp.
- **`Setelan` pill** opens Settings:
  - **WiFi**: SSID + password (on-screen keyboard), applied on save.
  - **Tampilan**: Floor and Room title fields.
  - **Layar & tema**: backlight dimmer (`−` / `+` / slider, 20–100 %) and the
    colour theme dropdown (applied live).
  - **Tombol lampu**: a **filter** (`Ruang ini` / `Lantai ini` / `Semua device`,
    with a `n dari m` counter), then per-button **device dropdown** (short ids
    such as `LAMP.14`) and **icon dropdown** (Spot / Umum / TL / Downlight /
    Taman).
  - **Simpan** persists everything to NVS and applies it.

Default assignment (8 lamps): `LAMP.14, LAMP.15, LAMP.09, LAMP.10, LAMP.11,
LAMP.12, LAMP.13, LAMP.17`.

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
python -m platformio run -e hioto_genmon -d firmware/hioto_general_monitor
python ota_publish.py firmware/hioto_general_monitor/.pio/build/hioto_genmon/firmware.bin `
    1.2.0 --device hioto-genmon --note "what changed"
python ota_trigger.py hioto-genmon-ec0940     # or wait for the 15 min check
```

`FW_VERSION`, `FW_DEVICE_TYPE` and `OTA_BASE` live in `include/config.h`.

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
- Touch calibration: Settings has no calibrate button; use the serial `cal`
  command (4 big numbered squares + centre validation, saved to NVS).
- Panel geometry: the ST7796 is 320×480 native; `TFT_WIDTH=320`,
  `TFT_HEIGHT=480` with `setRotation(1)` gives the 480×320 landscape UI.

## 2.8" CYD (ESP32-2432S028) � measured configuration

Verified against the unit with `firmware/cyd_probe` (see
`CYD-2432S028-CONTROL-REFERENCE.md`, Appendix B):

| Item | Value |
|---|---|
| Panel driver | **ST7789** (`-DST7789_DRIVER=1`) � ILI9341 renders black/snow |
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

Current status on the 2.8" unit: display and UI verified good; the touch read
returns X=0 / Y~4070 / Z=0 with IRQ stuck LOW for every implementation tried
(hardware VSPI, bit-banged on both clock edges, and a verbatim port of the probe
firmware), so the remaining fault is physical (FPC seating / touch controller),
not a build flag. A full power cycle and an FFC reseat are the next checks.

## Versions

| Version | Notes |
|---|---|
| 1.3.0 | first 2.8" build: ST7789, separate touch bus, per-panel `FW_DEVICE_TYPE` |
| 1.3.1-1.3.2 | calibration logging + `tp` diagnostic |
| 1.3.4-1.3.5 | XPT2046 read corrected to 3 transfers (cmd + hi + lo) |
| 1.3.6-1.3.8 | bit-banged touch, edge-variant diagnostics, verbatim probe port |