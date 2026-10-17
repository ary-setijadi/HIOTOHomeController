# HIOTO Secured Switch — Type A · v1.0

Firmware for the **Sunton ESP32-1732S019C** module
(ESP32-S3-WROOM-1, 16 MB flash, ST7789 170×320 SPI, GT911 touch — auto-probed, CH340 @ COM3).

- **Milestone 1 — DONE (verified on hardware):** display bring-up + interactive LVGL 8.4 demo UI.
- **Milestone 2 — current:** switch-device skeleton — NVS config, Wi-Fi provisioning
  captive portal, MQTT client (`iot/{id}/…` conventions), relay/switch output driver stub,
  live network status in the UI.

## Milestone 2 — switch device skeleton

### Module layout

| file | purpose |
|---|---|
| `src/main.cpp` | boot, display/LVGL, orchestration, BOOT button, backlight PWM |
| `src/settings.cpp` | NVS (`Preferences`, namespace `hioto`): Wi-Fi + MQTT + device id + relay GPIO + brightness |
| `src/wifi_mgr.cpp` | station mode + SoftAP captive-portal provisioning (`HIOTO-Setup`) |
| `src/mqtt_mgr.cpp` | MQTT client, LWT/status, telemetry, command handler |
| `src/relay.cpp` | relay/switch output driver stub (GPIO configurable, `-1` = simulated) |
| `src/ui.cpp` | LVGL UI: switch, brightness slider, network status |

### First boot / provisioning

No credentials stored → the device boots into the portal:

1. Connect your phone/laptop to Wi-Fi **`HIOTO-Setup`**, password **`CHANGE_ME`** (set via `PROV_AP_PASS` in `include/config.h`).
2. Open **http://192.168.4.1** (captive portal should prompt automatically).
3. Fill in Wi-Fi SSID/password and MQTT host/port/user/password (defaults pre-filled
   for the workspace RabbitMQ broker `192.168.137.44:1883`, user `admin`), press
   **Save & Reboot** — the portal stores everything in NVS and reboots.
4. On boot it joins your Wi-Fi and connects MQTT. The serial console shows progress.

### MQTT API (matches the `mqtt-simulator` topic scheme)

Device id: `hioto-XXXXXX` (derived from MAC, editable in the portal).

| topic | direction | payload |
|---|---|---|
| `iot/{id}/status` | device → broker, **retained** | `{"device":"…","status":"online"}` / `"offline"` (LWT) |
| `iot/{id}/telemetry` | device → broker, every 5 s + on state change | `{"device":"…","relay":false,"brightness":80,"uptime_s":123,"rssi":-55}` |
| `iot/{id}/commands` | broker → device, QoS1 | see below |

Commands (JSON, `{"cmd":"…"}`):

| cmd | effect |
|---|---|
| `{"cmd":"set","on":true}` | relay on/off; UI updates; state published |
| `{"cmd":"toggle"}` | toggle relay |
| `{"cmd":"brightness","value":60}` | set backlight 20–100 % (saved to NVS) |
| `{"cmd":"get"}` / `{"cmd":"state"}` | publish current state immediately |
| `{"cmd":"reboot"}` | restart the device |

Test from the workspace `mqtt-simulator` folder:

```powershell
cd mqtt-simulator
npx mqtt sub -h 192.168.137.44 -p 1883 -u admin -P 'CHANGE_ME' -t 'iot/+' -v
npx mqtt pub -h 192.168.137.44 -p 1883 -u admin -P 'CHANGE_ME' -t iot/hioto-XXXXXX/commands -m '{"cmd":"set","on":true}'
```

Or use the simulator dashboard (`npm.cmd run ui` → http://127.0.0.1:3000).

### Buttons / UI

| Element | I/O |
|---|---|
| Big switch | relay on/off (local + MQTT-synced), publishes state |
| Brightness slider | backlight PWM on GPIO14, persisted to NVS on release |
| BOOT (GPIO0) short press | toggles relay + publishes |
| BOOT (GPIO0) hold ≥ 4 s | factory reset: clears NVS, reboots into the portal |
| Status line | `AP: HIOTO-Setup` / `WiFi OK · MQTT OK` / `WiFi: connecting…` |

### Relay / switch output

The basic board has no relay yet — output is **simulated** (`relay_gpio = -1`, logs only).
When the output hardware is wired (SSR/relay driver), set its GPIO either in
`include/config.h` (`DEFAULT_RELAY_GPIO`) or in the provisioning portal (`relay` field);
the driver then drives the pin with `RELAY_ACTIVE_LEVEL`.

## Verified on this unit (bring-up findings)

- **Flash:** 16 MB confirmed (`flash=16384KB`).
- **PSRAM:** none detected (`psram=0KB` with both `qio_opi` and `qio_qspi` memory
  types) — LVGL draw buffers fall back to internal RAM; fine for 170×320.
- **Display SPI fix:** on ESP32-S3 with Arduino core 2.x, TFT_eSPI's default `FSPI`
  index resolves to 0 inside the IDF `REG_SPI_BASE()` macro, which crashes
  `TFT_eSPI::init()` with `StoreProhibited (addr 0x10)`. The build sets
  **`-DUSE_HSPI_PORT`** (valid register window) — mandatory for this combination.
- **Touch:** no GT911 answered a full 7-bit address scan on 13 candidate I2C bus
  orientations (18/17, 8/9, 41/42, 16/15, 4/5, 47/48, 38/39, both ways). If your
  panel is the C variant, provide the seller's touch-bus pinout — add the pair to
  `kBusCandidates` in `src/touch.cpp`. Until then use the BOOT button.

## Quick start

```powershell
cd firmware/hioto_switch

python -m platformio run                 # build (both envs)
python -m platformio run -e hioto_1732s019c -t upload   # flash demo/device on COM3
python -m platformio device monitor      # serial console @115200
```

**Hardware design document:** see [`docs/hardware-design.md`](docs/hardware-design.md)
(block diagram, HLK-PM01 power budget, pin map, BOM, mains safety notes).

`[env:smoke]` is a minimal no-display serial test used during bring-up
(`-e smoke -t upload`).

## Pin map (ST7789, verified mapping for the S3 variant)

| Signal | GPIO |
|---|---|
| MOSI | 13 |
| SCLK | 12 |
| CS | 10 |
| DC | 11 |
| RST | 1 |
| BL | 14 (HIGH = on) |
| BOOT | 0 |

### Alternate pin maps

If the screen stays white/blank: try `CS=15, SCLK=14`; swap `-DTFT_RGB_ORDER`;
remove `-DTFT_INVERSION_ON` if the image is negative; keep SPI at 40 MHz until stable.

## Notes

- Toolchain: PlatformIO Core (`pip install platformio`), `espressif32@6.9.0`
  (Arduino core 2.x), LVGL 8.4, TFT_eSPI 2.5.43, PubSubClient, ArduinoJson.
- `upload_port = COM3` is pinned so flashing never targets another CH340 adapter.
- Power: the HLK-PM01 (5 V) supply section belongs to the hardware design pass;
  firmware already assumes 5 V → onboard 3.3 V.
