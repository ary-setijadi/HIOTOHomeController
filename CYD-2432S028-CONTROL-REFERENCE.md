# ESP32 "CYD" 2.8" (ESP32-2432S028) — Complete Control Reference

Everything needed to drive every function on the board: display, touch, storage,
LEDs, sensors, audio, power, and flashing.

> **Verdict on the premise:** the 2.8" CYD family comes with **two different display
> controllers**. The classic single-micro-USB **ESP32-2432S028R** uses **ILI9341**;
> the newer **dual-USB** (micro-USB + USB-C) board uses **ST7789**. Both are
> 240×320 SPI panels with the *same* pinout — only the init sequence differs.
> Compiling the ILI9341 driver for an ST7789 board produces a black or garbled
> screen with no error message. See [§3](#3-which-controller-do-i-actually-have)
> to settle it with certainty.

---

## Table of contents

1. [What the board is](#1-what-the-board-is)
2. [Variant identification table](#2-variant-identification-table)
3. [Which controller do I actually have?](#3-which-controller-do-i-actually-have)
4. [Power and electrical](#4-power-and-electrical)
5. [Complete pin map](#5-complete-pin-map)
6. [GPIO constraints you must respect](#6-gpio-constraints-you-must-respect)
7. [Display control](#7-display-control)
8. [TFT_eSPI configuration](#8-tft_espi-configuration)
9. [LovyanGFX configuration](#9-lovyangfx-configuration)
10. [Touch (XPT2046)](#10-touch-xpt2046)
11. [microSD card](#11-microsd-card)
12. [RGB LED, LDR, speaker, buttons](#12-rgb-led-ldr-speaker-buttons)
13. [Free GPIOs and expansion](#13-free-gpios-and-expansion)
14. [Flashing and the "Wrong boot mode 0x13" trap](#14-flashing-and-the-wrong-boot-mode-0x13-trap)
15. [Copy-paste starter code](#15-copy-paste-starter-code)
16. [Troubleshooting matrix](#16-troubleshooting-matrix)
17. [References](#17-references)

---

## 1. What the board is

| Property | Value |
|---|---|
| Model | ESP32-2432S028 (printed in gold on the back, next to the speaker connector) |
| SoC | ESP32 (usually ESP32-WROOM-32 / D0WD, dual-core Xtensa LX6, 240 MHz) |
| Flash | 4 MB SPI (QIO) — *some* units ship 8/16 MB; verify with the probe |
| PSRAM | normally **none** on ESP32-WROOM-32 units; WROVER-based clones may have some |
| Radio | Wi-Fi 802.11 b/g/n 2.4 GHz + Bluetooth 4.2 / BLE |
| Display | 2.8" 240×320 TFT, SPI, no MISO needed for writes |
| Touch | XPT2046 resistive controller on a **dedicated SPI bus** |
| Storage | microSD slot (separate SPI bus from the display) |
| Extras | RGB status LED, LDR light sensor, speaker amp + 2P connector, BOOT + RESET buttons |
| USB-UART | CH340 (WCH) — `VID_1A86 PID_7523` |
| Power | 5 V via USB or P1 `VIN`; on-board 3.3 V LDOs |

The board exposes three 4-pin **1.25 mm Molex PicoBlade** connectors (`P1`, `P3`,
`CN1`) and one 2-pin speaker connector (`P4`). "mx1.25" JST connectors are the
usual cheap substitutes.

---

## 2. Variant identification table

| Board name | USB sockets | Controller | Touch | Notes |
|---|---|---|---|---|
| **ESP32-2432S028R** | 1 × micro-USB | **ILI9341** | XPT2046 resistive | The original "Cheap Yellow Display" |
| **ESP32-2432S028R** (dual) | micro-USB + USB-C | **ST7789** | XPT2046 resistive | Same silkscreen, different panel — the common trap |
| **ESP32-2432S028C** | micro-USB + USB-C | ILI9341 | **capacitive** (CST816/GT911) | Capacitive touch, I²C — XPT2046 code will not work |
| **JC2432W328** | varies | ST7789 | varies | Closely related Sunton/Guition board |
| Clones | varies | ILI9341 *or* ST7789 | varies | Must be probed, never assumed |

**Rules of thumb (not guarantees):**
- One USB socket → almost always ILI9341.
- Two USB sockets → almost always ST7789.
- Capacitive touch (no stylus-friendly pressure, glass surface, I²C traffic) → `C` variant.
- A 3.5" board with 480×320 (ESP32-3248S035) is a **different** family (ST7796) — don't mix them up.

---

## 3. Which controller do I actually have?

Three methods, in increasing order of certainty.

### 3.1 Look at the silkscreen and the sockets
Check the gold text on the back for the exact model, and count the USB sockets.
This narrows the field but is not proof (clones misprint).

### 3.2 Colour-order tell (software-only, no probe)
Flash any known-good ILI9341 build (see [§8](#8-tft_espi-configuration)):
- **Image appears** → ILI9341 (`ILI9341_2_DRIVER`).
- **Black screen, backlight on** → wrong driver; retry with `ST7789_DRIVER`.
- **Garbled/snow with correct geometry** → wrong driver *pairing* (init ran partially).

### 3.3 Register-level ID probe (definitive) ✅
Command the panel directly and read its ID registers — no library, no guessing.
This is what `firmware/cyd_probe` in this workspace does.

| Register | Command | Expected answer |
|---|---|---|
| `RDDID` | `0x04` | ST7789 → `0x00 0x85 0x85`, ST7796 → `0x00 0x77 0x96` |
| `RDID4` | `0xD3` | ILI9341 → `0x00 0x93 0x41`, ILI9488 → `0x00 0x94 0x88` |

> Command `0xD3` is the ILI9341 ID read. `0xDA` is **not** it — reading `0xDA`
> returns garbage on this panel and will send you chasing a wiring fault that
> does not exist.

Interpretation: **`0x9341` in the `0xD3` read ⇒ ILI9341.** **`0x8585` in the `0x04`
read ⇒ ST7789.** ILI9341 returns zeros for `0x04`; ST7789 returns zeros for `0xD3`.
That asymmetry is what makes the pair decisive.

Two practical details learned the hard way on this board:
- Send `SLPOUT (0x11)` before reading; ID reads are unreliable while the panel is
  asleep.
- Keep MISO at **1 MHz**. At 8 MHz the same read came back as `0xC0D9FF` noise.

Minimal raw-read snippet:

```cpp
SPIClass spi(HSPI);
spi.begin(14, 12, 13, 15);                     // SCLK, MISO, MOSI, CS
spi.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));

auto cmd = [&](uint8_t c){ digitalWrite(2, LOW);  digitalWrite(15, LOW);
                           spi.transfer(c);       digitalWrite(15, HIGH); };

auto read = [&](uint8_t c, uint8_t n) -> uint32_t {
  uint32_t v = 0;
  digitalWrite(2, LOW);  digitalWrite(15, LOW);  spi.transfer(c);
  digitalWrite(2, HIGH); spi.transfer(0);                    // dummy clock
  for (uint8_t i = 0; i < n; i++) v = (v << 8) | spi.transfer(0);
  digitalWrite(15, HIGH);
  return v;
};

cmd(0x01); delay(150);                          // SWRESET
cmd(0x11); delay(120);                          // SLPOUT - reads need the panel awake
uint32_t id04 = read(0x04, 3);                  // ST77xx family here
uint32_t idD3 = read(0xD3, 3);                  // ILI9xxx family here (0xD3, not 0xDA)
```

---

## 4. Power and electrical

| Item | Detail |
|---|---|
| Input | 5 V DC via either USB socket, or `VIN` on **P1** |
| Logic level | **3.3 V** — 5 V on any GPIO destroys the SoC |
| On-board rails | 3.3 V LDO for the ESP32 and a second LDO for the panel |
| Current | panel backlight dominates; budget ~250–350 mA typical, peaks higher |
| Test points | `S1` GND (near USB-serial), `S2` 3.3 V (ESP32), `S3` 5 V, `S4` GND (ESP32), `S5` 3.3 V (TFT) |
| LDO jumpers | `JP0` (panel LDO) and `JP3` (ESP32 LDO) are solder pads selectable between **5 V** and **3.3 V** — leave at factory settings unless you have a measured reason to change them |

**Practical notes**
- Backfeeding `VIN` while USB is attached is common but avoid driving both.
- The backlight is the biggest current draw; if you see brown-out resets when the
  panel is at full brightness, add bulk capacitance or lower brightness via PWM.
- Never power the board from 5 V on a GPIO; the P3/CN1 connectors carry 3.3 V only.

---

## 5. Complete pin map

### 5.1 Display (SPI — **HSPI** peripheral)

| Signal | GPIO | Notes |
|---|---|---|
| `TFT_MOSI` / SDI | **13** | |
| `TFT_SCLK` / SCK | **14** | |
| `TFT_MISO` / SDO | **12** | only needed for reads (ID probe, screen grab) |
| `TFT_CS` | **15** | strapping pin — has on-board pull-up |
| `TFT_DC` / RS | **2** | strapping pin |
| `TFT_RST` | — (tied to `EN`) | pass `TFT_RST -1` to the driver |
| `TFT_BL` (backlight) | **21** | **active HIGH**; plain GPIO, PWM-dimmable |

### 5.2 Touch — XPT2046 (dedicated bus, **not** the display bus)

| Signal | GPIO | Notes |
|---|---|---|
| `TP_CLK` | **25** | |
| `TP_MOSI` / DIN | **32** | |
| `TP_MISO` / DOUT | **39** | input only |
| `TP_CS` | **33** | |
| `TP_IRQ` | **36** | input only, **LOW when touched**, no internal pull-ups |

### 5.3 microSD (SPI — **VSPI** peripheral)

| Signal | GPIO |
|---|---|
| `SD_CS` | **5** (strapping pin) |
| `SD_SCK` | **18** |
| `SD_MISO` | **19** |
| `SD_MOSI` | **23** |

### 5.4 Everything else

| Function | GPIO | Notes |
|---|---|---|
| RGB LED — Red | **4** | **active LOW** |
| RGB LED — Green | **16** | active LOW |
| RGB LED — Blue | **17** | active LOW |
| LDR (light sensor) | **34** | input only, ADC1_CH6 — safe with Wi-Fi on |
| Speaker (via amp) | **26** | DAC2, feed with I²S DAC or `dacWrite()` |
| BOOT button | **0** | active LOW, has on-board pull-up; also a usable input |

### 5.5 Connectors

**P3** — 4P, general GPIO:

| Pin | Use | Note |
|---|---|---|
| 1 | GND | |
| 2 | **IO35** | input only, no pull-ups |
| 3 | **IO22** | also present on CN1 |
| 4 | **IO21** | *is the backlight* — not really free |

**CN1** — 4P, best I²C breakout:

| Pin | Use | Note |
|---|---|---|
| 1 | GND | |
| 2 | **IO22** | SCL candidate |
| 3 | **IO27** | SDA candidate |
| 4 | **3.3 V** | |

**P1** — 4P, serial/power: `VIN`, `TX` (IO1), `RX` (IO3), `GND`.

**P4** — 2P speaker connector, driven through the on-board amplifier (not a raw GPIO).

---

## 6. GPIO constraints you must respect

These are the things that silently break a build:

1. **Input-only pins: 34, 35, 36, 39.** No output, no internal pull-up/pull-down.
   The LDR lives on 34 and touch IRQ on 36 for exactly this reason.
2. **ADC2 is unusable while Wi-Fi is on.** ADC2 covers GPIO 2, 4, 12, 13, 14, 15,
   25, 26, 27 and `0`. Only **ADC1** (GPIO 32–39) is safe for analog reads in a
   networked sketch — which is why the LDR is on 34.
3. **Strapping pins** — the board already drives them, but do not add external
   circuitry that fights them at boot:
   - `GPIO0` BOOT — LOW at reset = download mode.
   - `GPIO2` (`TFT_DC`), `GPIO12` (`TFT_MISO`), `GPIO15` (`TFT_CS`), `GPIO5` (`SD_CS`).
   - `GPIO12` is the flash-voltage select (`MTDI`); it must not be pulled HIGH at
     boot or the ESP32 may boot into an unusable flash-voltage mode.
4. **GPIO21 is the backlight.** Any sketch that blindly drives "free" pins can kill
   the display. It is also broken out on P3, which lures people into using it.
5. **GPIO6–11** are wired to the on-board SPI flash — never touch them.
6. Pull-ups on `EN`/boot circuitry: don't load `EN` or `IO0` externally.

---

## 7. Display control

### 7.1 Geometry and orientation
The panel is natively **240 (W) × 320 (H) portrait**. Everything you see in
examples is `setRotation(1)` → **320 × 240 landscape**. Rotations 0 and 2 are
portrait, 1 and 3 landscape.

### 7.2 Driver settings that actually matter

| Setting | ILI9341 units | ST7789 units | Symptom when wrong |
|---|---|---|---|
| Driver macro | `ILI9341_2_DRIVER` | `ST7789_DRIVER` | black screen or snow |
| `TFT_RGB_ORDER` | `TFT_BGR` (typical) | `TFT_RGB` (typical) | red/blue swapped |
| Inversion | off | often off; some need `TFT_INVERSION_ON` | photo-negative image |
| `SPI_FREQUENCY` | 40–80 MHz | 40–80 MHz | flicker, tearing, artefacts |

> `ILI9341_2_DRIVER`, not plain `ILI9341_DRIVER`: the CYD ILI9341 panel needs the
> alternative init sequence (upstream issue Bodmer/TFT_eSPI#1172). This is the
> single most common "my CYD is blank" cause.

### 7.3 Backlight
`GPIO21`, **active HIGH**, straight from a GPIO — no driver IC. For dimming use
LEDC PWM:

```cpp
ledcSetup(0, 5000, 8);       // channel 0, 5 kHz, 8-bit
ledcAttachPin(21, 0);
ledcWrite(0, 255);           // 0 = off … 255 = full
```

PWM brightness can cause visible flicker/banding in photos; 5 kHz and above is
usually invisible.

### 7.4 SPI bus choice
The display is wired to the **HSPI** peripheral (that is why
`-DUSE_HSPI_PORT` appears in every working config). Define that macro or the
driver will try FSPI/VSPI and produce a blank screen.

---

## 8. TFT_eSPI configuration

Configure entirely from build flags (`-DUSER_SETUP_LOADED=1` bypasses the
library's `User_Setup.h`, so you never edit files inside `.pio/libdeps`).

### 8.1 PlatformIO — dual-USB (ST7789) variant

```ini
build_flags =
  -DUSER_SETUP_LOADED=1
  -DUSE_HSPI_PORT
  -DST7789_DRIVER=1
  -DTFT_WIDTH=240
  -DTFT_HEIGHT=320
  -DTFT_MOSI=13
  -DTFT_SCLK=14
  -DTFT_MISO=12
  -DTFT_CS=15
  -DTFT_DC=2
  -DTFT_RST=-1
  -DTFT_BL=21
  -DTFT_BACKLIGHT_ON=HIGH
  -DTFT_RGB_ORDER=TFT_RGB
  -DSPI_FREQUENCY=40000000
  -DSPI_READ_FREQUENCY=20000000
  -DLOAD_GLCD=1
  -DLOAD_FONT2=1
  -DLOAD_FONT4=1
```

### 8.2 PlatformIO — single-USB (ILI9341) variant

Identical, except:

```ini
  -DILI9341_2_DRIVER=1
  -DTFT_RGB_ORDER=TFT_BGR
```

### 8.3 Arduino IDE
Copy `DisplayConfig/User_Setup.h` from the ESP32-Cheap-Yellow-Display repo into
`Documents/Arduino/libraries/TFT_eSPI/`, then edit the driver macro for your
panel. Select *ESP32 Dev Module*; lower upload speed to 115200 if uploads fail.

### 8.4 How the config maps to behaviour
- `TFT_WIDTH/TFT_HEIGHT` = native portrait dims (240×320), **not** the landscape
  size you see on screen.
- `TFT_RST -1` is correct: the panel reset is tied to the module `EN` line.
- Keep `SPI_FREQUENCY` at 40 MHz while debugging, raise to 55–80 MHz once stable.

---

## 9. LovyanGFX configuration

Useful when you want runtime-switchable drivers, DMA, or better sprite handling.

```cpp
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX_CYD : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789  _panel;      // or Panel_ILI9341
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;

 public:
  LGFX_CYD() {
    { auto c = _bus.config();
      c.spi_host   = SPI2_HOST;      // HSPI
      c.spi_mode   = 0;
      c.freq_write = 40000000;
      c.freq_read  = 20000000;
      c.spi_3wire  = false;
      c.use_lock   = true;
      c.dma_channel= SPI_DMA_CH_AUTO;
      c.pin_sclk   = 14;
      c.pin_mosi   = 13;
      c.pin_miso   = 12;
      c.pin_dc     = 2;
      _bus.config(c);
      _panel.setBus(&_bus); }

    { auto c = _panel.config();
      c.pin_cs      = 15;
      c.pin_rst     = -1;
      c.pin_busy    = -1;
      c.panel_width = 240;
      c.panel_height= 320;
      c.invert      = false;         // flip for "negative" panels
      c.rgb_order   = false;         // true swaps R/B
      c.offset_x    = 0;
      c.offset_y    = 0;
      _panel.config(c);
      _panel.setRotation(1); }

    { auto c = _light.config();
      c.pin_bl     = 21;
      c.invert     = false;          // backlight is active HIGH
      c.freq       = 44100;
      c.pwm_channel= 7;
      _light.config(c);
      _panel.setLight(&_light); }

    setPanel(&_panel);
  }
};
```

---

## 10. Touch (XPT2046)

### 10.1 The bus problem, and the three ways out

The touch controller is **not** on the display SPI bus. It has its own five pins
(25/32/39/33/36). The SD card uses the **VSPI** peripheral on 18/19/23/5. The
ESP32 has only two usable hardware SPI peripherals and the display already owns
HSPI — so **touch and SD compete for VSPI**.

| Strategy | How | Trade-off |
|---|---|---|
| **A. Touch on hardware VSPI** | `SPIClass(VSPI)` at 1–2.5 MHz, SD unused | simplest; SD unavailable |
| **B. SD on hardware VSPI, touch bit-banged** | `XPT2046_Bitbang_Slim` | both work; tiny CPU cost, touch is slow |
| **C. Time-multiplex** | `SD.end()` → re-init VSPI for touch → swap back | both work; mode-switch latency |

The widely used recipe for "display + touch + SD simultaneously" is **B**.

### 10.2 Reading touch without a library

```cpp
SPIClass tp(VSPI);
tp.begin(25, 39, 32, 33);                       // SCLK, MISO, MOSI, CS
tp.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));

auto xpt = [&](uint8_t cmd) -> uint16_t {
  digitalWrite(33, LOW);
  uint8_t hi = tp.transfer(cmd);
  uint8_t lo = tp.transfer(0x00);
  digitalWrite(33, HIGH);
  return (uint16_t)((((uint16_t)hi << 8) | lo) >> 3) & 0x0FFF;   // 12-bit
};

uint16_t rawX = xpt(0xD0);   // X
uint16_t rawY = xpt(0x90);   // Y
uint16_t rawZ = xpt(0xB0);   // pressure
bool     down = (digitalRead(36) == LOW);
```

Raw range is 0…4095 but real panels use only ~300…3900. **Never hard-code
assumptions — calibrate.**

### 10.3 Calibration (do this once, store the result)

1. Draw four targets at known screen coordinates (corners inset ~20 px).
2. Have the user tap each; record raw X/Y.
3. Fit a linear map per axis:

```
screenX = (rawX - rawXmin) * screenW / (rawXmax - rawXmin)
screenY = (rawY - rawYmin) * screenH / (rawYmax - rawYmin)
```

4. Swap X/Y and/or mirror an axis if the fit is inverted — XPT2046 axes are not
   guaranteed to match panel axes across variants.
5. Persist the four raw extremes (and swap/mirror flags) in NVS
   (`Preferences`) so calibration survives reboots.

Expect `xMin ≈ xMax` swapped on some boards, and pressure `rawZ` to sit near 0
when untouched.

### 10.4 Capacitive variants
The `C` variant has no XPT2046 — it uses a CST816/GT911 on I²C (usually CN1's
IO22/IO27). Detect it by presence of I²C traffic at 0x15/0x5D. Resistive-touch
code will simply read nothing.

---

## 11. microSD card

```cpp
SPIClass sd(VSPI);
sd.begin(18, 19, 23, 5);
if (SD.begin(5, sd, 10000000)) {
  Serial.printf("type=%d size=%llu MB\n",
                SD.cardType(), SD.cardSize() / (1024ULL * 1024ULL));
}
```

- Works out of the box with the display (different peripheral), but conflicts with
  hardware-SPI touch (see §10.1).
- 10 MHz is a safe start; many cards do 20–25 MHz reliably. Long/unshielded card
  adapters or cheap cards need lower clocks.
- An empty slot is the normal state — `SD.begin()` returning false is not a fault.

---

## 12. RGB LED, LDR, speaker, buttons

### 12.1 RGB LED — active LOW
HIGH = off, LOW = on. All three off at boot is the safe default *after* you set
them; they float at reset.

```cpp
pinMode(4, OUTPUT); pinMode(16, OUTPUT); pinMode(17, OUTPUT);
digitalWrite(4, HIGH); digitalWrite(16, HIGH); digitalWrite(17, HIGH);  // all off
digitalWrite(16, LOW);   // green ON
```

These three pins are the most commonly sacrificed ones when a project needs more
IO — they are not used by the display.

### 12.2 LDR on GPIO34
Input-only, ADC1, **no on-board pull-up** — do not enable one. Higher raw value =
more light (photoresistor to 3.3 V, fixed resistor to GND). Use it as a relative
brightness signal; treat absolute numbers as unit-specific.

### 12.3 Speaker on GPIO26 (through the amp)
The 2P `P4` connector feeds an amplifier, so GPIO26 is an *audio input*, not a
switchable pin for external use.

```cpp
// crude square-wave tone, no library
for (int i = 0; i < 100; i++) { dacWrite(26, (i & 1) ? 128 : 0); delay(1); }
dacWrite(26, 0);
```

For real audio use the I²S DAC mode (`i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN)`
in IDF / `ESP32-audioI2S`-style libraries). Note GPIO25 is the other DAC pin but
it is taken by touch CLK, so only DAC2 (26) is available.

### 12.4 BOOT button on GPIO0
Active LOW with an on-board pull-up. Usable as a user button *after* boot — but
never hold it during reset or you enter download mode. RESET is a separate
button wired to `EN`.

---

## 13. Free GPIOs and expansion

Genuinely free, easily reachable pins:

| Pin | Where | Constraint |
|---|---|---|
| **IO35** | P3 | input only |
| **IO22** | P3 + CN1 | also on both connectors |
| **IO27** | CN1 | |
| **IO21** | P3 | ⚠ is the backlight — using it means no display |

Recommended: use **CN1** for I²C (IO22 + IO27, plus 3.3 V and GND) — that is a
complete, well-powered I²C port.

Need more pins? Options in ascending pain:
1. Sacrifice the RGB LED (IO4, IO16, IO17) → **+3 GPIOs**.
2. I²C expander (PCF8574 / MCP23017) on CN1 → +8 to +16, no desoldering.
3. An SD-slot "sniffer" breakout to reuse the SD lines.
4. Desolder the touch controller to reclaim IO25/32/33 (rarely worth it).

Avoid: IO6–11 (flash), IO34/35/36/39 (input only), and anything strapping.

---

## 14. Flashing and the "Wrong boot mode 0x13" trap

### 14.1 The symptom
```
A fatal error occurred: Failed to connect to Espressif device:
Wrong boot mode detected (0x13)! The chip needs to be in download mode.
```

This is a **known CYD hardware defect**, not a tooling problem: the capacitor
between `EN` and GND (`C4`, or `C5` on Type-C revisions) is 0.1 µF, which is too
small for the auto-reset timing that esptool uses. Whether auto-flash works is a
lottery across PC, OS, driver and board revision.

### 14.2 Immediate workaround — manual download mode
1. Hold **BOOT**.
2. Tap **RESET** (or briefly unplug/replug USB).
3. Release **BOOT**.
4. Flash with auto-reset disabled:

```powershell
# esptool v5 syntax
esptool --port COM3 --before no-reset --after hard-reset flash-id
```
```powershell
# esptool v4 syntax (underscores)
esptool.py --port COM3 --before no_reset --after hard_reset flash_id
```

PlatformIO equivalent — start the upload, and when it prints `Connecting...`
perform the BOOT+RESET sequence:

```powershell
pio run -e st7789 -t upload --upload-port COM3
```

Keep `upload_speed = 115200` for reliability; raise it only after a successful run.

### 14.3 Permanent fix — the capacitor mod
Replace the `EN`→GND capacitor `C4` (`C5` on Type-C revisions) with **1–10 µF**.
That restores reliable auto-reset. This is a physical modification; do it only
with proper SMD tools.

### 14.4 Backup before you flash anything
```powershell
esptool --port COM3 --before no-reset --after no-reset read-flash 0 0x400000 cyd_stock_backup.bin
```
(`read_flash` on esptool v4.) Do this **once, before the first flash**, so you can
always restore the factory demo.

### 14.5 Restoring a backup
```powershell
esptool --port COM3 --before no-reset --after hard-reset write-flash 0x0 cyd_stock_backup.bin
```

### 14.6 Windows/COM port notes
- The bridge is a **CH340** (`VID_1A86`, `PID_7523`); install the WCH driver if
  no COM port appears.
- Find it: `Get-CimInstance Win32_PnPEntity | Where-Object Name -match 'COM\d+'`
- Only one program may hold the port; close any serial monitor before flashing.
- On Linux, add yourself to `dialout` and disable `brltty` (it grabs CH340s).

---

## 15. Copy-paste starter code

### 15.1 Minimal display bring-up

```cpp
#include <TFT_eSPI.h>
TFT_eSPI tft = TFT_eSPI();

void setup() {
  tft.init();
  tft.setRotation(1);                       // 320x240 landscape
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("CYD alive", 10, 10, 2);

  // primary-colour bars: instant RGB-order / inversion sanity check
  tft.fillRect(0,   50, 80, 30, TFT_RED);
  tft.fillRect(80,  50, 80, 30, TFT_GREEN);
  tft.fillRect(160, 50, 80, 30, TFT_BLUE);
  tft.fillRect(240, 50, 80, 30, TFT_WHITE);
}
void loop() {}
```

If red shows as blue → flip `TFT_RGB_ORDER`. If white shows as black → add
`TFT_INVERSION_ON`. If nothing shows at all with the backlight lit → wrong driver
macro, go back to [§3](#3-which-controller-do-i-actually-have).

### 15.2 Backlight fade + touch read (no extra libraries)

```cpp
#include <SPI.h>

SPIClass tp(VSPI);                                   // touch lives on its own bus

void setup() {
  Serial.begin(115200);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(21, 0);                              // backlight PWM

  pinMode(33, OUTPUT); digitalWrite(33, HIGH);       // TP_CS
  pinMode(36, INPUT);                                // TP_IRQ
  tp.begin(25, 39, 32, 33);                          // SCLK, MISO, MOSI, CS
}

void loop() {
  static uint8_t b = 0;
  ledcWrite(0, b++);                                 // slow ~5 s fade

  if (digitalRead(36) == LOW) {                      // LOW = touched
    tp.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(33, LOW);
    uint8_t hi = tp.transfer(0xD0);                  // start X conversion
    uint8_t lo = tp.transfer(0x00);
    digitalWrite(33, HIGH);
    tp.endTransaction();
    Serial.printf("rawX=%u\n", (uint16_t)((((uint16_t)hi << 8) | lo) >> 3) & 0x0FFF);
  }
  delay(20);
}
```

Note `tp.begin(25, 39, 32, 33)` remaps VSPI to the touch pins — that *is* the
whole trick, since touch is not on the display bus.

---

## 16. Troubleshooting matrix

| Symptom | Most likely cause | Fix |
|---|---|---|
| Black screen, backlight on | wrong driver macro | §3 probe → `ST7789_DRIVER` vs `ILI9341_2_DRIVER` |
| Black screen, no backlight | GPIO21 not driven / used elsewhere | set 21 HIGH first |
| Colours wrong (red↔blue) | RGB/BGR order | flip `TFT_RGB_ORDER` |
| Image is a negative | panel inversion | add `TFT_INVERSION_ON` |
| Image upside-down / mirrored | rotation | `setRotation(1)`; check `MADCTL` |
| Flicker, tearing, sparse pixels | SPI too fast, weak USB | drop to 40 MHz, better cable/supply |
| Touch gives nothing | wrong bus / no IRQ pull | §10.2; IRQ 36 is LOW when pressed |
| Touch mirrored or swapped axes | uncalibrated | §10.3, persist in NVS |
| SD fails while touch works | VSPI contention | §10.1 strategy B or C |
| Upload fails, "0x13" | EN cap too small | §14.2 manual BOOT+RESET |
| Reboots when backlight bright | supply sag | lower brightness, better 5 V source |
| Wi-Fi + analog reads garbage | ADC2 in use | use ADC1 (32–39) only |

---

## 17. References

- ESP32 Cheap Yellow Display community docs — <https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display>
  (pin map, `User_Setup.h`, troubleshooting incl. the `0x13` capacitor issue)
- CYD variant/driver table and single- vs dual-USB rule — <https://github.com/AndroidCrypto/ESP32_CYD_Display_with_Touch_and_SD_Card>
- CYD hardware diagnostic tool (runtime ILI9341⇄ST7789 switching, full report) — <https://github.com/HenrysCat/ESP32-2432S028-CYD-Diagnostic-Tool>
- TFT_eSPI (driver macros, `ILI9341_2_DRIVER` rationale) — <https://github.com/Bodmer/TFT_eSPI>
- XPT2046 bit-bang library for touch+SD coexistence — <https://github.com/TheNitek/XPT2046_Bitbang_Arduino_Library>
- NuttX ESP32-2432S028 board notes — <https://nuttx.apache.org/docs/latest/platforms/xtensa/esp32/boards/esp32-2432S028/>

---

## Appendix A — local probe firmware

`firmware/cyd_probe/` in this workspace is a ready-to-build probe that performs
the register-level ID read and reports every peripheral:

```powershell
cd firmware\cyd_probe
pio run -e st7789          # dual-USB board
pio run -e ili9341         # single-USB board
pio run -e st7789 -t upload --upload-port COM3   # BOOT+RESET when Connecting...
pio device monitor -p COM3 -b 115200
```

It prints the controller, both ID registers, chip/flash/heap, SD, touch raw
values, LDR, BOOT state, and the exact `TFT_eSPI` defines to use for your panel.

<!-- LIVE-RESULTS -->

---

## Appendix B — Measured on this specific unit

Values below were read off the physical board with `firmware/cyd_probe`, not
copied from a datasheet.

### SoC and memory

| Property | Measured |
|---|---|
| Chip | **ESP32-D0WD-V3**, revision **v3.1** |
| Cores / clock | 2 cores + LP core, 240 MHz, 40 MHz crystal |
| Flash | **4 MB**, JEDEC `0x204016`, 3.3 V strap |
| PSRAM | **none** |
| SDK | v4.4.7 (Arduino core 2.x line) |
| MAC | `7C:D5:44:3D:42:CC` |

### Panel — verdict

**The panel is ST7789, not ILI9341.** Proof: the probe drew its report with
`ST7789_DRIVER` at 40 MHz, `TFT_RGB_ORDER=TFT_RGB`, no inversion, `setRotation(1)`,
and the image came out **clear and correctly coloured** on the physical screen.

This also matches the dual-USB variant rule in [§2](#2-variant-identification-table).
An ILI9341 build on this board produces **nothing at all** (backlight on, black
screen) — no error, no warning, just a blank panel.

**Register-level ID is not available on this unit.** Reading `0x04` and `0xD3`
returned noise (`0xC0D9FF`, `0xFFFFFF`) at 8 MHz *and* at 1 MHz with `SLPOUT`
applied, i.e. the panel's SDO/MISO line does not carry valid data back. On boards
like this the only reliable identification is the driver test described in
[§3.2](#32-colour-order-tell-software-only-no-probe) — which is exactly what
settled it here.

### Peripheral state

| Subsystem | Measured |
|---|---|
| microSD | slot empty — `GO_IDLE_STATE failed`, expected with no card |
| Touch | **Working** — XPT2046 on 25/32/39/33, IRQ 36. Verified: x 1078–2373, y 636–3244, z 400–1300 while pressing |
| LDR (GPIO34) | `0` in a dark room with occasional 80–160 noise spikes → sensor present, unlit |
| BOOT (GPIO0) | idle HIGH |
| RGB LED | cycled cleanly (active LOW confirmed) |

### Pre-existing firmware on this unit, and the bug it had

The board was **not** a blank demo — it was running a production
`HIOTO General Monitor` build (`hioto-genmon-28`, Living Room, MQTT online,
floor/room registered with the controller at `192.168.1.22`).

Its `firmware/hioto_general_monitor/platformio.ini` → `[env:hioto_genmon28]` had
**two hardware errors**, both of which this reference explains:

| Setting | Was | Should be | Effect of the bug |
|---|---|---|---|
| `-DILI9341_DRIVER=1` | ILI9341 | **`-DST7789_DRIVER=1`** | panel stayed black |
| `-DTFT_RGB_ORDER=TFT_BGR` | BGR | **`TFT_RGB`** (ST7789 default) | would have swapped red/blue once lit |
| `-DTOUCH_CS=33` | touch on the *display* bus | XPT2046 on **25/32/39/33**, IRQ 36 | `tcal NOT_FOUND` — touch never worked |
| `-DSPI_FREQUENCY=27000000` | 27 MHz | 40 MHz+ is fine | slower than necessary |

The touch error is the subtle one: `TOUCH_CS` only works when the XPT2046 sits on
the *same* SPI bus as the display — true for the 3.5" ESP32-3248S035 board, but
**not** for the 2.8" CYD, whose touch controller has its own five pins.

> **Corrected flags** for that env:
> ```ini
> -DST7789_DRIVER=1
> -DTFT_RGB_ORDER=TFT_RGB
> ; no TOUCH_CS - drive XPT2046 on its own bus (see §10)
> -DSPI_FREQUENCY=40000000
> ```

### Firmware backup taken before any flashing

`firmware/cyd_probe/backups/cyd_stock_backup.bin` — full 4 MB image,
verified at exactly 4,194,304 bytes. Restore with
`firmware/cyd_probe/tools/restore_stock.ps1`.

The restore was run and verified at the end of the session: the 4 MB image was
written back, esptool reported `Hash of data verified`, and the device rebooted
into `HIOTO General Monitor` with the same MAC/GUID and MQTT back online. So the
backup → probe → restore round trip is proven working on this board.

### Flashing behaviour observed

- Auto-reset genuinely does not work: `Wrong boot mode detected (0x13)`,
  reproducing the documented `C4`/`C5` capacitor issue in [§14](#14-flashing-and-the-wrong-boot-mode-0x13-trap).
- What *does* work: **hold BOOT, reconnect USB, release BOOT after ~2 s.**
  `--before no-reset` then connects on the first or second poll.
- `DTR`/`RTS` **reset** the chip (esptool reports *"Hard resetting via RTS pin"*)
  but cannot pull IO0 low — so a software-only entry into download mode is
  impossible on this board.
- **115200 is the only reliable baud rate here.** At 460800 the read aborted with
  `Serial data stream stopped` after ~48 kB. Expect ~6 min for a 4 MB dump.
- Reading flash immediately after a successful `flash-id` probe fails with
  `PermissionError(13, 'Access is denied')` unless the previous esptool process
  has fully released the COM handle — insert a 2–3 s gap.

### About the "garbage" on the panel

The noise/garble was **pre-existing, not a side effect of flashing.** It is
exactly what an ST7789 panel renders when initialised with ILI9341 commands:
backlight on, no error anywhere, and a screen full of noise (or nothing). The
backup → probe → restore cycle was byte-verified both ways, and the device
rejoined MQTT with its original GUID/MAC afterwards, so nothing was left behind.

One build flag separates "garbage" from a perfectly usable display.

### Touch — resolved: the bug was in the read, not the wiring

Touch works. The documented pinout is correct. The failure was a **driver bug in
my SPI read**, and it is worth documenting because the symptom is so misleading.

**The XPT2046 frame is 8 command clocks + 16 reply clocks.** The reply carries
`[dummy][12 data bits][3 zeros]`, so the 12-bit value is `(reply >> 3) & 0xFFF`.
The byte clocked *during* the command is **not** part of the reply:

```cpp
digitalWrite(TP_CS, LOW);
spi.transfer(cmd);                 // 8 clocks  - command
uint8_t hi = spi.transfer(0x00);   // 8 clocks  - dummy + bits 11..5
uint8_t lo = spi.transfer(0x00);   // 8 clocks  - bits 4..0 + zeros
digitalWrite(TP_CS, HIGH);
uint16_t v = (uint16_t)((((uint16_t)hi << 8) | lo) >> 3) & 0x0FFF;
```

Reading only **one** byte after the command (and treating the byte received
during the command as the high byte) truncates the value to its top 7 bits. The
tell was unmistakable once the raw words were logged:

```
irq Xword Yword Zword
1   0x807F 0x8065 0x8000     <- high byte stuck at 0x80 for every read
0   0x8039 0x803E 0x801D     <- all movement confined to the low byte
```

A constant `0x80` high byte with all variation in seven low bits is a truncated
read, **not** a dead bus, a wrong CS, or a floating MISO. Every decoded value
collapsed into 0–127, which is why the demo's "range learned" test never
unlocked and touch appeared dead.

Measured after the fix:

| State | x | y | z |
|---|---|---|---|
| untouched (IRQ high) | `0` | `4095` | `0` — garbage, and **normal** |
| pressed | 1078 – 2373 | 636 – 3244 | 400 – 1300 |

Two consequences for any driver you write:

1. **Always gate on the IRQ (GPIO36 low = contact).** An untouched XPT2046
   returns meaningless values (`0` / `4095`); feeding those into a min/max
   tracker destroys the calibration.
2. **Calibration is per-unit.** Record the raw extremes at the four corners,
   fit each axis linearly, and persist in NVS. Do not assume 0–4095.

The false trail this created is instructive: it looked exactly like a missing
controller, and the I²C scan, CS sweep and display-bus tests all "confirmed" that
suspicion because they all shared the same broken read. Logging the **raw words**
rather than the decoded value is what broke the deadlock.

### Demo UI that presents these findings

`firmware/cyd_demo/` is a touch/BOOT-driven UI that shows the findings on the
panel: the ST7789 verdict with a noise-vs-clean-colour side-by-side, SoC identity,
the full pin map, live XPT2046 telemetry with a crosshair, the wrong-vs-right
build flags, and live LDR / SD / BOOT / RGB LED checks.

```powershell
cd firmware\cyd_demo
pio run -e st7789                              # build the demo
powershell -File tools\flash_demo.ps1          # hold BOOT + reconnect USB
```

`-e ili9341_broken` deliberately builds the wrong driver so the garbage can be
reproduced on demand.
