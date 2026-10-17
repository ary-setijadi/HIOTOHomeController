#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "board_pins.h"
#include "touch.h"

/* ---------------------------------------------------------------------------
 *  GT911 touch controller — minimal, self-contained driver + bus scanner.
 *
 *  The vendor does not document the I2C pins of the ESP32-1732S019C, so at
 *  boot we probe a short list of likely (SDA, SCL) pairs (both orientations).
 *  On every candidate bus we run a full 7-bit address scan and print any
 *  responding devices to the serial console — that pinpoints the real touch
 *  bus even if the controller uses a different address or needs a reset.
 *
 *  GT911 registers used:
 *    0x814E  status  : bit7 = data ready, low nibble = point count
 *    0x8150  point 1 : track-id, Xl, Xh, Yl, Yh, sizeL, sizeH, reserved
 *    0x8048  config  : XmaxL, XmaxH, YmaxL, YmaxH, ...
 * ------------------------------------------------------------------------- */

#define GT911_STATUS 0x814EU
#define GT911_POINT1 0x8150U
#define GT911_CFG    0x8048U

static const int8_t kBusCandidates[][2] = {
    {18, 17}, {17, 18}, {8, 9},  {9, 8},  {41, 42}, {42, 41},
    {16, 15}, {15, 16}, {4, 5},  {5, 4},  {47, 48}, {48, 47}, {38, 39},
};
static const size_t kBusCandidateCount = sizeof(kBusCandidates) / sizeof(kBusCandidates[0]);

static bool     s_begun = false;
static int      s_sda = -1;
static int      s_scl = -1;
static uint8_t  s_addr = 0;
static uint16_t s_xmax = 0;
static uint16_t s_ymax = 0;
static char     s_text[96] = "TOUCH: none";

static bool gt_read(uint8_t addr, uint16_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  const size_t got = Wire.requestFrom((int)addr, (int)n);
  if (got != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}

static bool gt_write_u8(uint8_t addr, uint16_t reg, uint8_t v) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

/* Scan the current bus; returns a bitmask-safe list of found addrs (small set). */
static size_t scan_bus(uint8_t *found, size_t max) {
  size_t n = 0;
  for (uint8_t a = 0x03; a < 0x78 && n < max; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      found[n++] = a;
    }
  }
  return n;
}

static void indev_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  (void)drv;
  if (s_addr != 0) {
    uint8_t st = 0;
    if (gt_read(s_addr, GT911_STATUS, &st, 1) && (st & 0x80)) {
      uint8_t d[8];
      if (gt_read(s_addr, GT911_POINT1, d, sizeof d)) {
        const uint16_t x = (uint16_t)(d[1] | ((uint16_t)d[2] << 8));
        const uint16_t y = (uint16_t)(d[3] | ((uint16_t)d[4] << 8));
        gt_write_u8(s_addr, GT911_STATUS, 0x00); /* clear status */
        data->point.x = s_xmax ? (int32_t)x * LCD_WIDTH / (int32_t)s_xmax : (int32_t)x;
        data->point.y = s_ymax ? (int32_t)y * LCD_HEIGHT / (int32_t)s_ymax : (int32_t)y;
        data->state = LV_INDEV_STATE_PRESSED;
        return;
      }
      gt_write_u8(s_addr, GT911_STATUS, 0x00);
    }
  }
  data->state = LV_INDEV_STATE_RELEASED;
}

bool touch_init(void) {
  Serial.print("[touch] probing I2C buses: ");
  for (size_t i = 0; i < kBusCandidateCount; i++) {
    const int8_t sda = kBusCandidates[i][0];
    const int8_t scl = kBusCandidates[i][1];

    if (s_begun) { Wire.end(); s_begun = false; }
    if (!Wire.begin((int)sda, (int)scl, 400000)) continue;
    s_begun = true;

    uint8_t found[12];
    const size_t n = scan_bus(found, sizeof found);
    if (n == 0) continue;

    Serial.printf("\n[touch] devices on SDA:%d SCL:%d ->", sda, scl);
    for (size_t k = 0; k < n; k++) Serial.printf(" 0x%02X", found[k]);
    Serial.println();

    /* Prefer the GT911's two possible addresses. */
    uint8_t addr = 0;
    for (size_t k = 0; k < n; k++) {
      if (found[k] == 0x5D || found[k] == 0x14) { addr = found[k]; break; }
    }
    if (addr == 0) continue;

    /* Sanity-read the config block before trusting the IC. */
    uint8_t cfg[6] = {0};
    if (!gt_read(addr, GT911_CFG, cfg, 6)) {
      Serial.printf("[touch] GT911 candidate 0x%02X did not answer register reads\n", addr);
      continue;
    }

    s_addr = addr;
    s_sda = sda;
    s_scl = scl;
    s_xmax = (uint16_t)(cfg[0] | ((uint16_t)cfg[1] << 8));
    s_ymax = (uint16_t)(cfg[2] | ((uint16_t)cfg[3] << 8));
    gt_write_u8(s_addr, GT911_STATUS, 0x00);

    snprintf(s_text, sizeof s_text, "TOUCH: GT911 @0x%02X SDA:%d SCL:%d %ux%u",
             s_addr, s_sda, s_scl, s_xmax, s_ymax);

    static lv_indev_drv_t s_indev_drv;
    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = indev_read_cb;
    lv_indev_drv_register(&s_indev_drv);
    Serial.printf("[touch] %s\n", s_text);
    return true;
  }
  Serial.println("\n[touch] no GT911 found on candidate buses");
  return false;
}

const char *touch_status_text(void) {
  return s_text;
}
