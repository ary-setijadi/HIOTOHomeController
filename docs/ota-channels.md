# HIOTO General Monitor — firmware & OTA layout

One code base, **two firmware images**, because the two CYD boards use different
display controllers. The images are not interchangeable.

| board | panel chip | resolution | PlatformIO env | OTA device type |
|---|---|---|---|---|
| Sunton **ESP32-3248S035** (3.5") | ST7796 | 480×320 landscape | `genmon_35_st7796` | `hioto-genmon-35-st7796` |
| Sunton **ESP32-2432S028** (2.8") | ST7789 | 320×240 landscape | `genmon_28_st7789` | `hioto-genmon-28-st7789` |

The OTA device type is compiled into the image (`FW_DEVICE_TYPE`) and names the
board *and* the panel controller, so an image can only ever be pulled by the
board it was built for.

## Build and release

```bash
python ota_release.py                    # build both targets, package, no upload
python ota_release.py --publish          # ... and publish both OTA channels
python ota_release.py --board 28 --publish
python ota_release.py --no-build --publish   # republish what is already built
```

`ota_release.py` builds each env, copies the image to `firmware/releases/` as
`<device-type>-<version>.bin`, writes `MANIFEST.json` (size + SHA-256 per image)
and regenerates `RELEASES.md`, then optionally uploads to the Orange Pi.

The packaged binaries are committed on purpose: the exact image running on a
board is in the repo next to the source that produced it.

## OTA server (Orange Pi, `192.168.1.99:8080`)

One channel per device type; each is a directory with `latest.json` and
`firmware/`:

```
/srv/ota/hioto-genmon-35-st7796/latest.json
/srv/ota/hioto-genmon-35-st7796/firmware/fw-hioto-genmon-35-st7796-2.0.0.bin
/srv/ota/hioto-genmon-28-st7789/latest.json
/srv/ota/hioto-genmon-28-st7789/firmware/fw-hioto-genmon-28-st7789-2.0.0.bin
```

A board requests `/api/latest?device=<type>&guid=<id>` and downloads
`/firmware/<type>/<file>`. `/srv/ota/CHANNELS.md` on the Pi holds the same
channel map for reference.

### Retired channels

Kept for rollback only — no board should follow them:

| channel | was | superseded by |
|---|---|---|
| `hioto-genmon` | legacy shared name | the two chipset channels |
| `hioto-genmon-35` | 3.5" before the chipset split | `hioto-genmon-35-st7796` |
| `hioto-genmon-28` | 2.8" before the chipset split | `hioto-genmon-28-st7789` |

A board still sitting on a retired channel migrates by itself: publish the new
image under the *old* channel once, let the board install it, and from then on it
reports the new type and follows the new channel. That is exactly what the
`MIGRATION:` publishes did for v2.0.0.

## Wrong-board protection

Flashing the 2.8" image onto a 3.5" board (or vice versa) leaves the screen blank
while the board is perfectly healthy — the init sequence targets the wrong
controller. Two guards:

- **Board tag** (deterministic): the first firmware to boot on a board stamps its
  device type into NVS. A later boot whose *board token* (`35` / `28`) disagrees
  prints `[board] !!! FIRMWARE TIDAK COCOK DENGAN BOARD INI !!!`. A channel rename
  (same board, new type string) is adopted silently instead of crying wolf.
- **`dispid`** command: RDDID probe, best effort. TFT_eSPI's `readcommand8()`
  needs the ILI9341 `0xD9` parameter-select trick, so ST7796/ST7789 answer with
  `0xFF` and the firmware says "tidak terbaca" rather than guessing.

## Touch calibration

`cal` runs a 5-point routine (four corners plus centre) and fits
`screen = A·raw + B` by least squares, which absorbs imprecise taps. `tptarget`
draws a crosshair and reports the error of the real touch path — a freehand "tap
the centre" of a blank screen reads ~30 px high and is not a measurement.

## Console reference (serial, 115200 8N1, DTR/RTS off)

```
cal | clearcal          5-point touch calibration / clear it
tptarget                accuracy check against a drawn crosshair
tptest                  live screen coordinates while tapping
tpdiag                  touch bus sweep + I2C scan for a capacitive variant
dispid                  display controller id probe (best effort)
slot <n> <LAMP.xx>      assign button n (1-8) to a device
status | id | ver | lamps | cat | parts
wifi <ssid> <pass> | bright <5-100> | theme [0-5] | scope [0-2]
ota | otaauto <0|1> | otaurl <url> | save | reboot
```
