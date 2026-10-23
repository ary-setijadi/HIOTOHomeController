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
  - **Tombol lampu**: per-button **device dropdown** (all `AKTUATOR` lamps from
    the HIOTO controller) and **icon dropdown** (Spot / Umum / TL / Downlight /
    Taman).
  - **Simpan** persists everything to NVS and applies it.

Default assignment (8 lamps): `LAMP.14, LAMP.15, LAMP.09, LAMP.10, LAMP.11,
LAMP.12, LAMP.13, LAMP.17`.

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

## Settings persistence (NVS namespace `hioto`)

`ssid`/`pass`, `mhost`/`mport`/`muser`/`mpass`, `devid`, `floor`, `room`,
`bright` (backlight %), `theme` (palette index), `tcal` (touch calibration), and
per-slot `s{i}g` (guid), `s{i}l` (short label), `s{i}i` (icon index).

## Serial console (COM3 @ 115200)

`help` · `status` · `wifi <ssid> <pass>` · `mqtt <host> <port> <user> <pass>` ·
`floor <label...>` · `room <label...>` · `bright [20-100]` · `theme [0-5]` ·
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
