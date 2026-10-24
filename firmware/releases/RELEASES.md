# HIOTO General Monitor — firmware releases

Two boards, two images. The panel controller differs, so the images are
not interchangeable: flashing the wrong one leaves the screen blank even
though the board is healthy (the firmware prints a board-tag warning).

| board | panel | OTA channel | env |
|---|---|---|---|
| ESP32-3248S035 (3.5") | ST7796 | `hioto-genmon-35-st7796` | `genmon_35_st7796` |
| ESP32-2432S028 (2.8") | ST7789 | `hioto-genmon-28-st7789` | `genmon_28_st7789` |

## Images

| version | device type | chip | size | sha256 | file |
|---|---|---|---|---|---|
| 2.0.0 | `hioto-genmon-28-st7789` | ST7789 | 1221344 | `15414edce41a0f73…` | `hioto-genmon-28-st7789-2.0.0.bin` |
| 2.0.0 | `hioto-genmon-35-st7796` | ST7796 | 1210864 | `503e60e32aabaf49…` | `hioto-genmon-35-st7796-2.0.0.bin` |

Rebuild and publish both:

```
python ota_release.py --publish
```

Cable-flash one board (manual download mode: hold BOOT while powering):

```
python -m platformio run -e genmon_35_st7796 -t upload
```
