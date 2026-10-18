# HIOTO General Monitor — lamp switch panel (v1.0)

Firmware for the **Sunton ESP32-3248S035** 3.5" CYD (ESP32-D0WD-V3, ST7796
320×480 native panel driven as 480×320 landscape, XPT2046 touch, CH340 @ COM3).

A **simple lamp switch panel**:

- Title bar **`[Floor] · [Room]`** (editable in Settings).
- **2 × 4 big buttons**, each with a lamp icon (spot / general), the lamp's short
  id (e.g. `LAMP.14`) and its live ON/OFF state. Tap = toggle the lamp.
- **`Setelan` pill** opens Settings:
  - **WiFi**: SSID + password (on-screen keyboard), applied on save.
  - **Tampilan**: Floor and Room title fields.
  - **Tombol lampu**: per-button **device dropdown** (all `AKTUATOR` lamps from
    the HIOTO controller) and **icon dropdown** (Spot / Umum).
  - **Simpan** persists everything to NVS and applies it.

Default assignment (8 lamps): `LAMP.14, LAMP.15, LAMP.09, LAMP.10, LAMP.11,
LAMP.12, LAMP.13, LAMP.17`.

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
`bright`, `tcal` (touch calibration), and per-slot `s{i}g` (guid), `s{i}l`
(short label), `s{i}i` (icon 0=spot, 1=general).

## Serial console (COM3 @ 115200)

`help` · `status` · `wifi <ssid> <pass>` · `mqtt <host> <port> <user> <pass>` ·
`floor <label...>` · `room <label...>` · `ldr` · `cal` / `clearcal` / `traw` ·
`save` · `reboot`

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
