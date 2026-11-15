# CYD Hardware Probe

Reads the **real** display controller ID out of the panel (register level) and
reports every on-board peripheral of an ESP32-2432S028 "Cheap Yellow Display".

Output goes to the serial port **and** the TFT, so it works even when the panel
driver choice is still unknown.

## Why

The 2.8" CYD ships with two different panels behind an identical pinout:

| Variant | USB sockets | Controller |
|---|---|---|
| ESP32-2432S028R (classic) | 1 × micro-USB | ILI9341 |
| ESP32-2432S028R (dual) | micro-USB + USB-C | ST7789 |

The probe settles it from the hardware instead of trusting the label.

## Build

```powershell
pio run -e st7789     # dual-USB board (ST7789)  <- start here
pio run -e ili9341    # single-USB board (ILI9341)
```

## Flash

This board **cannot auto-reset** — the EN capacitor (C4/C5) is too small, so
esptool fails with `Wrong boot mode detected (0x13)`.

1. Hold **BOOT**, tap **RESET** (or replug USB while holding BOOT), release **BOOT**.
2. Upload:

```powershell
pio run -e st7789 -t upload --upload-port COM3
```

If PlatformIO's own reset sequence still fails, flash the built binaries directly
while the board is in download mode:

```powershell
cd .pio\build\st7789
esptool --port COM3 --before no-reset --after hard-reset write-flash `
  0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

Keep the baud rate at 115200. At 460800 the CH340 link on this board drops
packets (`Serial data stream stopped`).

## Watch

```powershell
pio device monitor -p COM3 -b 115200
```

## What it reports

- SoC: chip model/revision, cores, CPU MHz, flash size + speed, PSRAM, heap,
  SDK version, MAC
- Display: controller classification from `RDDID (0x04)` + `RDID4 (0xDA)`
- microSD: card type, size, root entry count
- Touch: XPT2046 raw X/Y/Z, IRQ state
- RGB LED cycle, LDR raw value, BOOT button state, speaker chirp
- The exact `TFT_eSPI` defines to use for the detected panel

## Reading the ID registers

| Read | ILI9341 | ST7789 |
|---|---|---|
| `0x04` (RDDID) | `0x000000` | `0x008585` |
| `0xD3` (RDID4) | `0x009341` | `0x000000` |

The asymmetry is the proof: whichever register answers non-zero identifies the
family. `0xD3` is the ILI9341 ID command — `0xDA` is *not* and returns noise.

Read at **1 MHz** and send `SLPOUT (0x11)` first; at 8 MHz this panel's MISO line
returns garbage.

## Before first flash — back up the stock firmware

```powershell
esptool --port COM3 --baud 115200 --before no-reset --after no-reset `
  read-flash 0 0x400000 backups\cyd_stock_backup.bin
```

Restore with `write-flash 0x0 backups\cyd_stock_backup.bin`.
A verified 4 MB dump (`backups/cyd_stock_backup.bin`) is kept alongside this
project once taken.
