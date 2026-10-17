# HIOTO Floor Monitor — Type F · v1.0

Firmware for the **Sunton ESP32-3248S035** — the classic 3.5" CYD
(ESP32-D0WD-V3, 4 MB flash, ST7796 480×320 SPI display, XPT2046 resistive touch,
LDR, RGB LED, CH340 @ COM3).

A **HIOTO compound-floor monitor**: a wall/desk panel that represents one floor
of the compound (e.g. **Lantai 1**) and shows the live state of that floor's
devices with their human names, straight from the HIOTO controller.

## What it does

| Layer | Behavior |
|---|---|
| Display | 480×320 landscape, LVGL 8.4, dark theme, Indonesian labels |
| Wi-Fi | Station mode with NVS credentials; `HIOTO-Monitor-Setup` captive portal on first boot |
| MQTT | Publishes `iot/{id}/telemetry` (10 s), retained `iot/{id}/status` (LWT), plus HIOTO-compatible `Status` heartbeat and `sensor_suhu/<guid>` (every 30 s); subscribes `iot/...`, `Sensor`, `Status`, `sensor_suhu/+`, `sensor_gas_detector/+`, `sensor_water_tank/+`, `sensor_weather/+`, `Log/+` |
| Floor registry | Fetches `GET http://<ctrl>:8081/api/devices` once, keeps only this floor's devices (floor token `1ST`/`2ND`/`3RD`/`4TH` in the name, or matching `floor_id`), and uses their real names on the dashboard |
| Floor view | Room tabs (`Semua` + per-room + `Lainnya` last) built from the controller registry; each tab lists that room's devices **sorted by name** (up to 32 tracked, scrollable) |
| Heartbeat | shown on the **detail page** (ONLINE/OFFLINE + last seen); list rows stay clean |
| Lamp toggle | lamp/actuator rows (registry type `AKTUATOR`) get a tappable **ON/OFF pill** on the right; tapping it publishes the HIOTO `Aktuator` command (`<guid>#0` = ON, `#1` = OFF, active-low). Tapping the rest of the row opens the detail page. Lamp state is also learned from the controller's `Aktuator` broadcast, so a physical switch turning a lamp on/off shows up immediately (and Status heartbeats never clobber it) |
| Environment | Panel die temperature, LDR light % (approx. lux), RSSI, free heap |
| Touch | XPT2046 via TFT_eSPI; tap a row → detail page; on-screen calibration from the Info page |
| Buttons | BOOT short press cycles pages (Dashboard → Detail → Info); hold ≥ 4 s = factory reset |
| RGB LED | blue = setup AP, green = Wi-Fi + MQTT OK, amber = Wi-Fi only, red = offline |

## Pages

1. **Dashboard** — top bar (HIOTO · floor · Wi-Fi/MQTT), **room tabs** with the
   floor device list, environment and status cards, footer with `online/total` and uptime.
2. **Detail** — one node: status, kind, last value, age, id, big **Kembali**.
3. **Info** — device id, floor, Wi-Fi/IP, MQTT endpoint, MAC, flash/RAM, uptime,
   panel, registry status; **Kembali / Kalibrasi Layar / Reboot**.

## HIOTO controller integration (this deployment)

- MQTT broker: **192.168.1.22:1883**, user **`/smarthome:smarthome`**
  (RabbitMQ 3.8.3 `vhost:user` quirk), password `Ssm4rt2!`.
- Controller HTTP API: **192.168.1.22:8081** — `GET /api/devices`,
  `POST /api/register`.
- The panel registers itself once in the controller as
  `hioto-floor-ec0940` / type `SENSOR`, name `HIOTO FLOOR MONITOR L1`, floor_id 3
  (Lantai 1). The panel then publishes a `Status <guid>#1` heartbeat every 30 s
  and `sensor_suhu/<guid>` temperature JSON, so the controller can keep it
  online and chart its temperature.
  > Note: the running v4m controller builds its in-memory device map at start-up;
  > a newly registered device is picked up after the controller is restarted.

## First boot / provisioning

No credentials stored → the panel starts a SoftAP:

1. Connect your phone/laptop to Wi-Fi **`HIOTO-Monitor-Setup`**, password **`hioto1234`**.
2. Open **http://192.168.4.1** (captive portal should prompt automatically).
3. Fill in Wi-Fi SSID/password, MQTT host/port/user/password, the floor label
   (default **`Lantai 1`**), controller host/port + floor id, brightness;
   press **Save & Reboot**.
4. On boot it joins the Wi-Fi, connects MQTT, fetches the floor registry and
   starts publishing.

### Serial provisioning console (COM3 @ 115200)

`help` · `status` · `wifi <ssid> <pass>` · `mqtt <host> <port> <user> <pass>` ·
`ctrl <host> <port>` · `floor <label...>` · `floorid <n>` · `dev <id>` ·
`ldr` (raw LDR ADC) · `reg` (refetch registry) · `save` (persist + reboot) · `reboot`

## MQTT API

Device id: `hioto-floor-XXXXXX` (derived from the MAC, editable in the portal).

| topic | direction | payload |
|---|---|---|
| `iot/{id}/status` | device → broker, **retained** | `{"device":"…","status":"online","role":"floor-monitor","floor":"Lantai 1"}` / `"offline"` (LWT) |
| `iot/{id}/telemetry` | device → broker, every 10 s | `{"device":"…","role":"floor-monitor","floor":"Lantai 1","die_temp_c":53.3,"light_pct":0,"lux":0,"rssi":-45,"uptime_s":123,"heap":169896,"nodes":16,"online":16,"ip":"192.168.4.143"}` |
| `Status` | device → broker (HIOTO legacy) | `<guid>#1` heartbeat |
| `sensor_suhu/<guid>` | device → broker (HIOTO legacy) | `{"guid":"…","mac":"…","deviceName":"Lantai 1","value":{"temperature":53.3},"unit":{"temperature":"C"}}` |
| `iot/{id}/commands` | broker → device, QoS1 | see below |

Commands (JSON, `{"cmd":"…"}`):

| cmd | effect |
|---|---|
| `{"cmd":"get"}` / `{"cmd":"state"}` | publish telemetry immediately |
| `{"cmd":"brightness","value":60}` | backlight 20–100 % (saved to NVS) |
| `{"cmd":"floor","value":"Lantai 2"}` | change the floor label (saved, telemetry re-published) |
| `{"cmd":"reboot"}` | restart the device |

## Hardware facts (this unit, verified)

- **Chip**: ESP32-D0WD-V3 rev v3.1, dual core, 240 MHz, MAC `4c:c3:82:ec:09:40`.
- **Flash**: 4 MB (GigaDevice `c8 4016`), no PSRAM → `huge_app.csv` (3 MB app slot).
- **Display**: ST7796 480×320 over HSPI — MOSI 13, MISO 12, SCLK 14, CS 15, DC 2,
  RST tied to EN, BL 27.
- **Touch**: XPT2046 on the **shared display SPI bus** — CS 33, IRQ 36.
- **LDR**: GPIO34 · **RGB LED**: 4/16/17 (active LOW) · **BOOT**: GPIO0.

## Pin map

| Signal | GPIO |
|---|---|
| MOSI / MISO / SCLK | 13 / 12 / 14 |
| TFT CS / DC / BL | 15 / 2 / 27 |
| Touch CS | 33 |
| LDR | 34 |
| RGB LED R / G / B | 4 / 16 / 17 |
| BOOT | 0 |

## Flashing — IMPORTANT: manual download mode

This board's CH340 auto-reset does **not** enter download mode, so flash manually:

1. **Hold BOOT**, tap **RESET** once, keep BOOT held ~1 s, release.
2. Then flash with `--before no_reset --after hard_reset`:

```powershell
cd firmware/hioto_floor_monitor
python -m platformio run                      # build

python "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" --chip esp32 `
  --port COM3 --baud 921600 --before no_reset --after hard_reset `
  write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB `
  0x1000  .pio\build\hioto_3248s035\bootloader.bin `
  0x8000  .pio\build\hioto_3248s035\partitions.bin `
  0xe000  "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin" `
  0x10000 .pio\build\hioto_3248s035\firmware.bin

python -m platformio device monitor           # serial console @115200
```

`[env:smoke]` is a minimal no-display serial test (`-e smoke -t upload`, same manual
download-mode step).

## Troubleshooting

- **Blank/white screen** → try `-DTFT_RGB_ORDER=TFT_BGR`, or check that
  `USE_HSPI_PORT` is set (required for the HSPI pin group).
- **Touch offset** → run calibration: on the serial console type `cal` (or publish
  `{"cmd":"calibrate"}` to `iot/<id>/commands`), then tap the 5 crosses; the result
  is saved to NVS. The panel needs calibration on this unit because the top/bottom
  screen edges are otherwise unreachable.
- **Upside-down UI** → change `s_tft.setRotation(1)` in `main.cpp` to `3`.
- **No devices listed** → check `[reg]` log line on the serial console; the panel
  falls back to showing all MQTT devices when the registry fetch fails.

## Notes

- Toolchain: PlatformIO Core, `espressif32@6.9.0` (Arduino core 2.x), LVGL 8.4,
  TFT_eSPI 2.5.43, PubSubClient, ArduinoJson.
- The die temperature is the SoC sensor (board temperature), not room temperature;
  ambient light comes from the on-board LDR.
