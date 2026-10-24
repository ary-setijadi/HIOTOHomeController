#include "touch.h"

#include "board_pins.h"
#include "config.h"
#include "settings.h"
#include "ui.h"

#if TOUCH_SEPARATE_BUS

/* ---------------------------------------------------------------- 2.8" CYD */
#include <SPI.h>

#define TP_SCLK 25
#define TP_MOSI 32
#define TP_MISO 39
#define TP_CS 33
#define TP_IRQ 36

static TFT_eSPI *s_tft = nullptr;
static int s_w = 240, s_h = 320;
static bool s_cal_ok = false;
static uint16_t s_xmin = 200, s_xmax = 3900, s_ymin = 200, s_ymax = 3900;
static bool s_swap = false, s_invx = false, s_invy = false;

/* XPT2046 frame: 8 command clocks, then 16 reply clocks. The reply byte clocked
 * during the command is NOT part of the answer, so three transfers are needed â€”
 * this matches the probe firmware verified on this exact unit. */
/* Bit-banged XPT2046 read. The display owns a hardware SPI peripheral and the
 * touch chip sits on its own five pins, so driving it by hand removes every
 * bus-sharing question (the reference lists this as the standard recipe). */
static uint16_t xpt(uint8_t cmd) {
  digitalWrite(TP_CS, LOW);
  delayMicroseconds(2);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(TP_MOSI, (cmd >> i) & 1);
    delayMicroseconds(1);
    digitalWrite(TP_SCLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(TP_SCLK, LOW);
    delayMicroseconds(1);
  }
  uint16_t raw = 0;
  for (int i = 0; i < 16; i++) {
    digitalWrite(TP_SCLK, HIGH);
    delayMicroseconds(1);
    raw = (uint16_t)((raw << 1) | (digitalRead(TP_MISO) & 1));
    digitalWrite(TP_SCLK, LOW);
    delayMicroseconds(1);
  }
  digitalWrite(TP_CS, HIGH);
  return (uint16_t)((raw >> 3) & 0x0FFF);
}

static void load_calibration(void) {
  const uint16_t *c = g_settings.touch_cal;
  if (!g_settings.touch_cal_valid || c[1] <= c[0] || c[3] <= c[2]) {
    s_cal_ok = false;
    return;
  }
  s_xmin = c[0];
  s_xmax = c[1];
  s_ymin = c[2];
  s_ymax = c[3];
  s_swap = (c[4] & 1) != 0;
  s_invx = (c[4] & 2) != 0;
  s_invy = (c[4] & 4) != 0;
  s_cal_ok = true;
}

void touch_begin(TFT_eSPI *tft) {
  s_tft = tft;
  pinMode(TP_CS, OUTPUT);
  digitalWrite(TP_CS, HIGH);
  pinMode(TP_SCLK, OUTPUT);
  digitalWrite(TP_SCLK, LOW);
  pinMode(TP_MOSI, OUTPUT);
  digitalWrite(TP_MOSI, LOW);
  pinMode(TP_MISO, INPUT); /* input-only GPIO, no pull-ups available */
  pinMode(TP_IRQ, INPUT);
  load_calibration();
  Serial.printf("[touch] XPT2046 clk25/mosi32/miso39/cs33/irq36 cal=%s\n",
                s_cal_ok ? "ada" : "belum");
}

void touch_set_screen(int w, int h) {
  if (w > 0) s_w = w;
  if (h > 0) s_h = h;
}

bool touch_is_down(void) { return digitalRead(TP_IRQ) == LOW; }

uint16_t touch_raw_pressure(void) { return xpt(0xB0); }

/* unconditional sample for diagnostics: no thresholding, no gating */
void touch_debug_sample(uint16_t *z, uint16_t *x, uint16_t *y) {
  *z = xpt(0xB0);
  *x = xpt(0xD0);
  *y = xpt(0x90);
  *x = xpt(0xD0); /* second conversion is the stable one */
  *y = xpt(0x90);
}

bool touch_raw(uint16_t *rx, uint16_t *ry) {
  if (xpt(0xB0) < 40) return false; /* pressure check */
  uint32_t sx = 0, sy = 0;
  for (int i = 0; i < 4; i++) {
    sx += xpt(0xD0);
    sy += xpt(0x90);
  }
  *rx = (uint16_t)(sx / 4);
  *ry = (uint16_t)(sy / 4);
  return true;
}

bool touch_point(int *x, int *y) {
  if (!touch_is_down()) return false;
  uint16_t rx = 0, ry = 0;
  if (!touch_raw(&rx, &ry)) return false;

  const int a = s_swap ? (int)ry : (int)rx;
  const int b = s_swap ? (int)rx : (int)ry;

  int xx, yy;
  if (s_cal_ok) {
    xx = (a - (int)s_xmin) * (s_w - 1) / (int)(s_xmax - s_xmin);
    yy = (b - (int)s_ymin) * (s_h - 1) / (int)(s_ymax - s_ymin);
  } else { /* rough default until calibrated */
    xx = (a - 200) * (s_w - 1) / 3700;
    yy = (b - 200) * (s_h - 1) / 3700;
  }
  if (s_invx) xx = (s_w - 1) - xx;
  if (s_invy) yy = (s_h - 1) - yy;
  if (xx < 0) xx = 0;
  if (xx > s_w - 1) xx = s_w - 1;
  if (yy < 0) yy = 0;
  if (yy > s_h - 1) yy = s_h - 1;
  *x = xx;
  *y = yy;
  return true;
}


/* Diagnostic: read the same channel with two sampling edges so we can tell a
 * wiring problem from a clock-phase problem in one shot. */
uint16_t touch_debug_variant(uint8_t cmd, int mode) {
  digitalWrite(TP_CS, LOW);
  delayMicroseconds(5);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(TP_MOSI, (cmd >> i) & 1);
    delayMicroseconds(2);
    digitalWrite(TP_SCLK, HIGH);
    delayMicroseconds(2);
    digitalWrite(TP_SCLK, LOW);
    delayMicroseconds(2);
  }
  uint16_t raw = 0;
  for (int i = 0; i < 16; i++) {
    digitalWrite(TP_SCLK, HIGH);
    delayMicroseconds(2);
    if (mode == 0) raw = (uint16_t)((raw << 1) | (digitalRead(TP_MISO) & 1));
    digitalWrite(TP_SCLK, LOW);
    delayMicroseconds(2);
    if (mode == 1) raw = (uint16_t)((raw << 1) | (digitalRead(TP_MISO) & 1));
  }
  digitalWrite(TP_CS, HIGH);
  return (uint16_t)((raw >> 3) & 0x0FFF);
}
/* ------------------------------------------------------------------------- */
/*  Verbatim port of the probe firmware's touch read (own SPIClass instance,  */
/*  begin() once, 8 warm-up iterations) — used only to compare behaviour.     */
/* ------------------------------------------------------------------------- */
bool touch_probe_read(uint16_t *out_x, uint16_t *out_y, uint16_t *out_z) {
  static SPIClass *spi = nullptr;
  if (!spi) {
    pinMode(TP_CS, OUTPUT);
    digitalWrite(TP_CS, HIGH);
    pinMode(TP_IRQ, INPUT);
    spi = new SPIClass(VSPI);
    spi->begin(TP_SCLK, TP_MISO, TP_MOSI, TP_CS);
  }
  spi->beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  uint16_t x = 0, y = 0, z = 0;
  for (int i = 0; i < 8; i++) {
    digitalWrite(TP_CS, LOW);
    spi->transfer(0xD0);
    const uint8_t h1 = spi->transfer(0x00);
    const uint8_t l1 = spi->transfer(0x00);
    digitalWrite(TP_CS, HIGH);
    x = (uint16_t)((((uint16_t)h1 << 8) | l1) >> 3) & 0x0FFF;

    digitalWrite(TP_CS, LOW);
    spi->transfer(0x90);
    const uint8_t h2 = spi->transfer(0x00);
    const uint8_t l2 = spi->transfer(0x00);
    digitalWrite(TP_CS, HIGH);
    y = (uint16_t)((((uint16_t)h2 << 8) | l2) >> 3) & 0x0FFF;

    digitalWrite(TP_CS, LOW);
    spi->transfer(0xB0);
    const uint8_t h3 = spi->transfer(0x00);
    const uint8_t l3 = spi->transfer(0x00);
    digitalWrite(TP_CS, HIGH);
    z = (uint16_t)((((uint16_t)h3 << 8) | l3) >> 3) & 0x0FFF;
    delay(3);
  }
  spi->endTransaction();
  *out_x = x;
  *out_y = y;
  *out_z = z;
  return true;
}
#else /* ------------------------------------------------------- 3.5" CYD path */

static TFT_eSPI *s_tft = nullptr;

void touch_begin(TFT_eSPI *tft) {
  s_tft = tft;
  if (g_settings.touch_cal_valid) {
    s_tft->setTouch(g_settings.touch_cal);
    Serial.println("[touch] cal tersimpan dipakai");
  }
}

void touch_set_screen(int w, int h) {
  (void)w;
  (void)h;
}

bool touch_is_down(void) { return s_tft && s_tft->getTouchRawZ() > 300; }

bool touch_raw(uint16_t *rx, uint16_t *ry) {
  if (!s_tft) return false;
  uint16_t a = 0, b = 0;
  s_tft->getTouchRaw(&a, &b);
  *rx = a;
  *ry = b;
  return true;
}

bool touch_point(int *x, int *y) {
  if (!s_tft) return false;
  uint16_t px = 0, py = 0;
  if (!s_tft->getTouch(&px, &py, 400)) return false;
  *x = px;
  *y = py;
  return true;
}

uint16_t touch_raw_pressure(void) { return s_tft ? s_tft->getTouchRawZ() : 0; }

void touch_debug_sample(uint16_t *z, uint16_t *x, uint16_t *y) {
  *z = touch_raw_pressure();
  *x = 0;
  *y = 0;
  if (s_tft) s_tft->getTouchRaw(x, y);
}

#endif

/* ------------------------------------------------------------------------- */
/*  Shared 4-point calibration (both topologies)                             */
/* ------------------------------------------------------------------------- */
static bool wait_tap(uint16_t *rx, uint16_t *ry, uint32_t timeout_ms) {
  const uint32_t t0 = millis();
  uint32_t last_report = 0;
  while (millis() - t0 < timeout_ms) {
    if (millis() - last_report > 5000) {
      last_report = millis();
      Serial.printf("[cal] menunggu tap: irq=%d\n", (int)touch_is_down());
    }
    if (touch_is_down()) {
      uint32_t sx = 0, sy = 0;
      int n = 0;
      uint16_t a = 0, b = 0;
      while (n < 8 && touch_is_down()) {
        if (touch_raw(&a, &b)) {
          sx += a;
          sy += b;
          n++;
        }
        delay(15);
      }
      if (n > 0) {
        *rx = (uint16_t)(sx / n);
        *ry = (uint16_t)(sy / n);
        return true;
      }
    }
    delay(15);
  }
  return false;
}

void touch_calibrate_ui(void) {
  if (!s_tft) {
    Serial.println("[cal] tidak ada tft");
    return;
  }
  const int w = (int)s_tft->width();
  const int h = (int)s_tft->height();
  Serial.printf("[cal] mulai %dx%d rot=%u\n", w, h, (unsigned)g_settings.rot);
  const int inset = 24;
  const int px[4] = {inset, w - inset, inset, w - inset};
  const int py[4] = {inset, inset, h - inset, h - inset};

  s_tft->setRotation(g_settings.rot);
  s_tft->fillScreen(TFT_BLACK);
  s_tft->setTextColor(TFT_WHITE, TFT_BLACK);
  s_tft->drawString("KALIBRASI: ketuk 4 kotak", 6, 4, 2);
  for (int i = 0; i < 4; i++) {
    s_tft->fillRect(px[i] - 12, py[i] - 12, 24, 24, TFT_CYAN);
    s_tft->drawRect(px[i] - 14, py[i] - 14, 28, 28, TFT_WHITE);
  }

  uint16_t raw[4][2];
  for (int i = 0; i < 4; i++) {
    Serial.printf("[cal] ketuk kotak %d di (%d,%d)\n", i + 1, px[i], py[i]);
    if (!wait_tap(&raw[i][0], &raw[i][1], 60000)) {
      Serial.printf("[cal] BATAL di kotak %d\n", i + 1);
      s_tft->drawString("BATAL", w / 2 - 24, h / 2, 4);
      delay(900);
      ui_after_calibration();
      return;
    }
    s_tft->fillRect(px[i] - 12, py[i] - 12, 24, 24, TFT_GREEN);
    Serial.printf("[cal] ok %d raw=(%u,%u)", i + 1, raw[i][0], raw[i][1]);
    delay(120);
  }

  /* Are the controller axes swapped relative to the screen? */
  const int d_xtr = abs((int)raw[1][0] - (int)raw[0][0]);
  const int d_ytr = abs((int)raw[1][1] - (int)raw[0][1]);
  const bool swap = d_ytr > d_xtr;

  int xl, xr, yt, yb;
  if (!swap) {
    xl = (raw[0][0] + raw[2][0]) / 2;
    xr = (raw[1][0] + raw[3][0]) / 2;
    yt = (raw[0][1] + raw[1][1]) / 2;
    yb = (raw[2][1] + raw[3][1]) / 2;
  } else {
    xl = (raw[0][1] + raw[2][1]) / 2;
    xr = (raw[1][1] + raw[3][1]) / 2;
    yt = (raw[0][0] + raw[1][0]) / 2;
    yb = (raw[2][0] + raw[3][0]) / 2;
  }

  const bool invx = xl > xr;
  const bool invy = yt > yb;

  uint16_t c[8] = {0};
  c[0] = (uint16_t)(invx ? xr : xl); /* xMin */
  c[1] = (uint16_t)(invx ? xl : xr); /* xMax */
  c[2] = (uint16_t)(invy ? yb : yt); /* yMin */
  c[3] = (uint16_t)(invy ? yt : yb); /* yMax */
  c[4] = (uint16_t)((swap ? 1 : 0) | (invx ? 2 : 0) | (invy ? 4 : 0));

  memcpy(g_settings.touch_cal, c, sizeof c);
  g_settings.touch_cal_valid = true;
  settings_save(&g_settings);
  Serial.printf("[touch] cal x=%u..%u y=%u..%u swap=%d invx=%d invy=%d\n", c[0], c[1],
                c[2], c[3], (int)swap, (int)invx, (int)invy);

#if TOUCH_SEPARATE_BUS
  s_xmin = c[0];
  s_xmax = c[1];
  s_ymin = c[2];
  s_ymax = c[3];
  s_swap = swap;
  s_invx = invx;
  s_invy = invy;
  s_cal_ok = (c[1] > c[0] && c[3] > c[2]);
#else
  s_tft->setTouch(g_settings.touch_cal);
#endif

  /* centre check */
  s_tft->fillScreen(TFT_BLACK);
  s_tft->setTextColor(TFT_WHITE, TFT_BLACK);
  s_tft->drawString("ketuk titik TENGAH", w / 2 - 70, 4, 2);
  s_tft->drawCircle(w / 2, h / 2, 22, TFT_YELLOW);
  uint16_t cx = 0, cy = 0;
  if (wait_tap(&cx, &cy, 30000)) {
    int x = 0, y = 0;
    if (touch_point(&x, &y) && abs(x - w / 2) < 48 && abs(y - h / 2) < 48) {
      s_tft->setTextColor(TFT_GREEN, TFT_BLACK);
      s_tft->drawString("KALIBRASI OK", w / 2 - 62, h - 22, 2);
      delay(1200);
      ui_after_calibration();
      return;
    }
  }
  s_tft->setTextColor(TFT_RED, TFT_BLACK);
  s_tft->drawString("KALIBRASI GAGAL", w / 2 - 70, h - 22, 2);
  delay(1400);
  ui_after_calibration();
}
