# CYD Findings Demo UI

A touch/BOOT-driven UI that presents this unit's measured findings **on the panel
itself** — including a side-by-side of the noise you were seeing versus what the
same panel does with the correct driver.

## Screens

| # | Screen | Shows |
|---|---|---|
| 1 | **Findings** | `ST7789` in large type, `NOT ILI9341`, and a live noise patch ("what you saw") next to six clean colour bars ("what it can do") |
| 2 | **SoC / Memory** | chip, revision, flash, PSRAM, SDK, MAC, heap — plus the note that the ID registers return noise on this unit |
| 3 | **Pin map** | display (HSPI), touch (own bus), SD (VSPI), RGB LED, LDR, speaker, backlight |
| 4 | **Touch** | live raw x/y/z, IRQ state, learned min/max range, and a crosshair showing where your finger maps on screen |
| 5 | **Why it was garbage** | the wrong-vs-right build flags, and why `TOUCH_CS` cannot work on this board |
| 6 | **Peripherals** | live LDR bar, SD card detection, BOOT button state, RGB LED colour cycling |

## Navigation

- **BOOT button (GPIO0)** — advance to the next screen, wrapping. Always works.
- **Footer `<` / `>`** — tap to page. Touch works, but it needs a little range
  knowledge first: tap around the corners of the screen (the Touch screen shows
  the learned `range x .. / y ..`), after which `mapping: usable` appears and the
  footer buttons respond.

Touch notes worth knowing (these cost a lot of time to find):

- The frame is **8 command clocks + 16 reply clocks**. The byte clocked during
  the command is *not* part of the reply; reading only one byte afterwards
  truncates the value to its top 7 bits and makes the panel look dead.
- An untouched XPT2046 returns garbage (`x=0, y=4095`). Always gate on the IRQ
  line (GPIO36 low = contact) before trusting or recording a sample.
- Raw extremes differ per unit — observed here: x 1078–2373, y 636–3244.
  Calibrate rather than assuming 0–4095.
- If the crosshair moves the wrong way, swap X/Y or mirror an axis in
  `touchScreenX()` / `touchScreenY()`.

## Build and flash

```powershell
cd firmware\cyd_demo
pio run -e st7789
```

This board cannot auto-reset. Hold **BOOT**, reconnect USB, release **BOOT**
after ~2 s, then:

```powershell
pio run -e st7789 -t upload --upload-port COM3
```

`-e ili9341_broken` builds the *wrong* driver on purpose, so you can A/B the
garbage for yourself.

## Restoring the previous firmware

The pre-existing image is saved at
`../cyd_probe/backups/cyd_stock_backup.bin`. Restore with:

```powershell
powershell -File ..\cyd_probe\tools\restore_stock.ps1
```

(That image carries the ILI9341 misconfiguration, so it will show the same noise
again — it is the *original* state, not a fix.)

## Why this matters

The garbage was never a hardware fault and was not caused by flashing. It is what
an **ST7789** panel renders when it is initialised with **ILI9341** commands: the
backlight comes on and the panel shows noise or nothing, with no error anywhere.
One build flag is the difference.
