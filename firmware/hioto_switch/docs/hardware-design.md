# HIOTO Secured Switch — Type A · v1.0
## Hardware Design Document

Status: **draft v1.0** — firmware-side facts are verified on the actual unit; the
mains/power section is still a paper design pending the PCB pass.

---

## 1. Overview

A wall-mounted touch switch ("Type A") built on an ESP32-S3 display module:

- **Compute + display:** Sunton **ESP32-1732S019C** module
  (ESP32-S3-WROOM-1, 16 MB flash, ST7789 170×320 SPI TFT, GT911 touch option,
  CH340 USB-UART, 5 V USB-C input with onboard 3.3 V regulator)
- **UI:** LVGL 8.4 HMI (firmware: `firmware/hioto_switch`)
- **Connectivity:** Wi-Fi (2.4 GHz) + MQTT (`iot/{id}/…` scheme)
- **Power:** **HLK-PM01** AC/DC module (90–264 VAC → 5 V / 0.6 A)
- **Switching output:** not on the basic board yet — a relay/SSR driver is the
  next hardware addition (firmware already supports it via configurable GPIO,
  simulated when unset).

## 2. Block diagram

```
  ┌──────────────────────────────────────────────────────────────────┐
  │  AC MAINS  (L / N, 100–240 VAC)                                  │
  │      │                                                           │
  │      ├── F1  fuse  1 A (slow-blow)        ── safety, mandatory   │
  │      │                                                           │
  │      ├─────────────────────────────────┐                        │
  │      │                 HLK-PM01         │                        │
  │      │   AC in ───────────────▶ 5 V out │──┬── 5 V rail          │
  │      │   (isolated 3000 VAC)            │  │                     │
  │      └─────────────────────────────────┘  │                     │
  │                                           ├── bulk cap 470 µF ──┤
  │                                           │                     │
  │   ESP32-1732S019C module  ◀── USB-C (5V, CH340 data) ────────────┤
  │   ├── onboard 3.3 V LDO ──▶ ESP32-S3-WROOM-1                    │
  │   ├── ST7789 170×320 SPI  (GPIO 10–14, 1)                       │
  │   ├── GT911 touch I2C     (bus TBD on module revision)          │
  │   └── backlight NMOS + PWM (GPIO 14)                            │
  │                                                                 │
  │   (future) relay / SSR driver  ◀── GPIO (configurable)          │
  │      └── load contacts ──▶ switched mains load                  │
  └──────────────────────────────────────────────────────────────────┘
```

**Isolation boundary:** the HLK-PM01 is the only mains↔SELV barrier. Everything
after its 5 V output is SELV (≤ 5 V DC) and can be handled as low voltage.

## 3. Power budget (verified module current draw + estimates)

| Load | Rail | Typ | Peak / notes |
|---|---|---|---|
| ESP32-S3 Wi-Fi TX bursts | 3.3 V (from 5 V LDO) | ~120 mA | **~240–350 mA peak** during TX |
| ESP32-S3 idle/RX | 3.3 V | ~40–60 mA | |
| ST7789 panel logic | 3.3 V | ~5 mA | |
| Backlight (LED string via PWM) | 3.3 V | 15–35 mA | 20–100 % duty |
| GT911 touch (if present) | 3.3 V | 2–5 mA | |
| CH340 (only while USB attached) | 5 V | 15–25 mA | |
| **Module total, 5 V input** | 5 V | **~90–150 mA** | **≤ ~420 mA peak** |

- **HLK-PM01 rating: 5 V / 0.6 A (3 W)** → steady-state headroom ≈ 4×,
  worst-case (Wi-Fi TX + backlight) ≈ 70 % of rating.
- **Adding a relay coil** (SRD-05VDC-SL-C ≈ 70–80 mA @ 5 V) pushes peaks to
  ~500 mA — still inside 0.6 A, but add a **470 µF bulk capacitor** near the
  relay/PM01 output and prefer a **latching relay or SSR** if the load topology
  allows it.
- HLK-PM01 has internal short-circuit/over-current protection with self-recovery;
  still keep F1 (1 A slow-blow) in the AC input for wiring faults.

## 4. Pin map (verified from firmware bring-up)

The firmware sets these via `-D` build flags in `platformio.ini` and simulates
`TFT_eSPI` register access; the values below are confirmed working on hardware.

| Signal | GPIO | Notes |
|---|---|---|
| LCD MOSI | 13 | SPI data |
| LCD SCLK | 12 | SPI clock |
| LCD CS | 10 | |
| LCD DC | 11 | |
| LCD RST | 1 | |
| LCD BL | 14 | PWM backlight, active HIGH |
| BOOT button | 0 | user input / factory reset (4 s hold) |
| Touch SDA/SCL | TBD | GT911 not found on probed buses — confirm the module's revision pinout |
| Relay output | **TBD** | free GPIO e.g. 4/5/6/7/15/16/17/18; set in `config.h` or portal |

### Firmware-side quirks worth remembering

- **`-DUSE_HSPI_PORT` is mandatory** for TFT_eSPI on ESP32-S3 with Arduino core
  2.x (default FSPI index resolves to register base 0 → `StoreProhibited`).
- The unit reports **0 KB PSRAM**; LVGL buffers run from internal RAM (OK at
  170×320 with 40-line buffers).

## 5. BOM (Type A v1.0)

| # | Item | Qty | Notes |
|---|---|---|---|
| 1 | Sunton ESP32-1732S019C module (ESP32-S3, 16 MB flash, ST7789 1.9") | 1 | verified: `flash=16384KB`, no PSRAM on this batch |
| 2 | Hi-Link **HLK-PM01** AC/DC 5 V 0.6 A | 1 | 90–264 VAC input, 3000 VAC isolation |
| 3 | Fuse F1 1 A slow-blow + holder | 1 | AC input protection |
| 4 | MOV varistor (S07K275 / S10K275) | 1 | optional surge protection across L-N |
| 5 | Bulk capacitor 470 µF / 16 V | 1 | 5 V rail, relay/transient buffer |
| 6 | Screw terminal (3-way) L / N / load | 1–2 | mains + load wiring |
| 7 | Relay module or SSR | 1 | **future** output; driver pin configurable |
| 8 | Enclosure for 86-type wall box, Class II | 1 | double insulation, no exposed metal |

## 6. Safety notes (mains design)

- HLK-PM01 provides 3000 VAC isolation — it is the **only** mains-side component
  allowed to cross the barrier. Keep mains traces ≥ 6 mm from SELV traces;
  add a PCB slot/cutout under the module for creepage if the layout permits.
- Fuse F1 in the AC line is mandatory for wiring faults.
- The enclosure must be Class II (double insulation): no exposed conductive parts,
  no user-accessible screws touching the mains side.
- Label the wall box: "L / N / load" and note that the unit must not be opened
  while energized.
- The HLK-PM01 is rated for a 3 W continuous load; respect the 470 µF bulk cap on
  the 5 V side if a relay is added later.

## 7. Open items / next hardware additions

1. Confirm the module's **touch revision** (C vs N) and its I2C pinout — then
   enable touch in firmware (`kBusCandidates` → exact bus + GT911 init).
2. Choose the switching element (mechanical relay vs SSR) and its GPIO.
3. PCB pass: HLK-PM01 section with fuse/MOV/bulk cap, display module header,
   relay driver, mains terminals — then DRC/DFM and a production package.
4. Enclosure fit test (86-type box).
