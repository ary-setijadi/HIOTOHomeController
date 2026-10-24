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
 * during the command is NOT part of the answer, so three transfers are needed —
 * this matches the probe firmware verified on this exact unit. */
/* Bit-banged XPT2046 read. The display owns a hardware SPI peripheral and the
 * touch chip sits on its own five pins, so driving it by hand removes every
 * bus-sharing question (the reference lists this as the standard recipe). */
static uint16_t xpt_once(uint8_t cmd) {
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

/* Averaged read. A single conversion right after a channel switch can return
 * the previous channel's residue, which makes one axis look dead while the
 * other is fine. Discard the first conversion, settle, then average three. */
static uint16_t xpt(uint8_t cmd) {
  xpt_once(cmd);
  delayMicroseconds(50);
  uint32_t sum = 0;
  for (int i = 0; i < 3; i++) sum += xpt_once(cmd);
  return (uint16_t)(sum / 3);
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
/*  begin() once, 8 warm-up iterations) � used only to compare behaviour.     */
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
/*  Shared 5-point calibration (both topologies)                             */
/* ------------------------------------------------------------------------- */
static bool wait_tap(uint16_t *rx, uint16_t *ry, uint32_t timeout_ms) {
  const uint32_t t0 = millis();
  uint32_t last_report = 0;

  /* A finger may still be resting from the previous box. Wait for it to lift,
   * otherwise one press answers two boxes and the fit is built from twice the
   * same raw position. */
  const uint32_t rel0 = millis();
  while (touch_is_down() && millis() - rel0 < 3000) delay(10);

  while (millis() - t0 < timeout_ms) {
    if (millis() - last_report > 5000) {
      last_report = millis();
      Serial.printf("[cal] menunggu tap: irq=%d\n", (int)touch_is_down());
    }
    if (touch_is_down()) {
      delay(40); /* let the contact settle before sampling */
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
      /* stay here until the finger lifts, so the next box needs a fresh tap */
      const uint32_t rel1 = millis();
      while (touch_is_down() && millis() - rel1 < 2000) delay(10);
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

/* Least-squares fit of  screen = A*raw + B  over the collected taps, then
 * extrapolated to the two screen edges. Returns the raw value that corresponds
 * to screen 0 as *edge_a and to screen (span-1) as *edge_b, either of which may
 * be larger depending on how the panel is mirrored. */
static bool touch_fit_axis(const uint16_t *r, const int *t, int n, int span, uint16_t *edge_a,
                           uint16_t *edge_b) {
  if (n < 3) return false;
  double sr = 0, st = 0, srr = 0, srt = 0;
  for (int i = 0; i < n; i++) {
    sr += r[i];
    st += t[i];
    srr += (double)r[i] * (double)r[i];
    srt += (double)r[i] * (double)t[i];
  }
  const double den = (double)n * srr - sr * sr;
  if (fabs(den) < 1e-6) return false; /* all taps on one spot */
  const double A = ((double)n * srt - sr * st) / den;
  const double B = (st - A * sr) / (double)n;
  if (fabs(A) < 1e-9) return false;

  const double r0 = (0.0 - B) / A;                 /* raw at screen 0 */
  const double rN = ((double)(span - 1) - B) / A;  /* raw at the last pixel */
  if (!(r0 > -800.0 && r0 < 4900.0 && rN > -800.0 && rN < 4900.0)) return false;
  const double lo = r0 < rN ? r0 : rN;
  const double hi = r0 < rN ? rN : r0;
  if (hi - lo < 400.0) return false; /* implausibly small range for a 12-bit axis */

  double a = r0, b = rN;
  if (a < 0) a = 0;
  if (a > 4095) a = 4095;
  if (b < 0) b = 0;
  if (b > 4095) b = 4095;
  if (fabs(a - b) < 400.0) return false;
  *edge_a = (uint16_t)(a + 0.5);
  *edge_b = (uint16_t)(b + 0.5);
  return true;
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
  s_tft->drawString("KALIBRASI: ketuk 5 kotak", 6, 4, 2);
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

  /* ---- 5th tap: the centre. It is both the check and a fit sample ------- */
  const int cxp = w / 2, cyp = h / 2;
  Serial.printf("[cal] ketuk kotak 5 di (%d,%d)\n", cxp, cyp);
  s_tft->fillRect(cxp - 12, cyp - 12, 24, 24, TFT_CYAN);
  s_tft->drawRect(cxp - 14, cyp - 14, 28, 28, TFT_WHITE);
  uint16_t rc[2] = {0, 0};
  const bool have_centre = wait_tap(&rc[0], &rc[1], 60000);
  if (have_centre) {
    s_tft->fillRect(cxp - 12, cyp - 12, 24, 24, TFT_GREEN);
    Serial.printf("[cal] ok 5 raw=(%u,%u)\n", rc[0], rc[1]);
    delay(120);
  } else {
    Serial.println("[cal] pusat dilewati, memakai 4 sudut saja");
  }

  /* Are the controller axes swapped relative to the screen? */
  const int d_xtr = abs((int)raw[1][0] - (int)raw[0][0]);
  const int d_ytr = abs((int)raw[1][1] - (int)raw[0][1]);
  const bool swap = d_ytr > d_xtr;

  /* Collect one sample per known screen position. Four corners alone force the
   * fit through whatever the user actually hit; adding the centre tap and
   * fitting by least squares averages those aiming errors out instead. */
  const int n_pts = have_centre ? 5 : 4;
  uint16_t rvx[5], rvy[5];
  int tx[5], ty[5];
  for (int i = 0; i < 4; i++) {
    rvx[i] = swap ? raw[i][1] : raw[i][0];
    rvy[i] = swap ? raw[i][0] : raw[i][1];
    tx[i] = px[i];
    ty[i] = py[i];
  }
  if (have_centre) {
    rvx[4] = swap ? rc[1] : rc[0];
    rvy[4] = swap ? rc[0] : rc[1];
    tx[4] = cxp;
    ty[4] = cyp;
  }

  uint16_t xa = 0, xb = 0, ya = 0, yb = 0;
  const bool fit_x = touch_fit_axis(rvx, tx, n_pts, w, &xa, &xb);
  const bool fit_y = touch_fit_axis(rvy, ty, n_pts, h, &ya, &yb);

  uint16_t c[8] = {0};
  bool invx, invy;
  if (fit_x && fit_y) {
    /* touch_point() divides by (max - min), so the stored pair must ascend;
     * invx/invy carry the "which screen edge did that raw value belong to". */
    invx = xa > xb; /* raw at screen x=0 is larger -> mirrored */
    invy = ya > yb;
    c[0] = xa > xb ? xb : xa;
    c[1] = xa > xb ? xa : xb;
    c[2] = ya > yb ? yb : ya;
    c[3] = ya > yb ? ya : yb;
  } else {
    /* least squares failed (degenerate taps): fall back to the corner averages */
    Serial.println("[cal] fit gagal, pakai rata-rata sudut");
    int xl, xr, yt, yb2;
    if (!swap) {
      xl = (raw[0][0] + raw[2][0]) / 2;
      xr = (raw[1][0] + raw[3][0]) / 2;
      yt = (raw[0][1] + raw[1][1]) / 2;
      yb2 = (raw[2][1] + raw[3][1]) / 2;
    } else {
      xl = (raw[0][1] + raw[2][1]) / 2;
      xr = (raw[1][1] + raw[3][1]) / 2;
      yt = (raw[0][0] + raw[1][0]) / 2;
      yb2 = (raw[2][0] + raw[3][0]) / 2;
    }
    invx = xl > xr;
    invy = yt > yb2;
    c[0] = (uint16_t)(invx ? xr : xl);
    c[1] = (uint16_t)(invx ? xl : xr);
    c[2] = (uint16_t)(invy ? yb2 : yt);
    c[3] = (uint16_t)(invy ? yt : yb2);
  }
  c[4] = (uint16_t)((swap ? 1 : 0) | (invx ? 2 : 0) | (invy ? 4 : 0));

  memcpy(g_settings.touch_cal, c, sizeof c);
  g_settings.touch_cal_valid = true;
  settings_save(&g_settings);
  Serial.printf("[touch] cal x=%u..%u y=%u..%u swap=%d invx=%d invy=%d (fit=%d/%d)\n", c[0],
                c[1], c[2], c[3], (int)swap, (int)invx, (int)invy, (int)fit_x, (int)fit_y);

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

  /* The centre tap is already recorded, so grade the fit against it instead of
   * asking for another tap. Anything above ~5 % of the axis is worth a retry. */
  s_tft->fillScreen(TFT_BLACK);
  s_tft->setTextColor(TFT_WHITE, TFT_BLACK);
  s_tft->drawString("KALIBRASI: cek", 6, 4, 2);
  s_tft->drawCircle(w / 2, h / 2, 22, TFT_YELLOW);

  int ex = 0, ey = 0;
  bool ok = false;
  if (have_centre) {
    const int ax = swap ? rc[1] : rc[0];
    const int ay = swap ? rc[0] : rc[1];
    int pxr = (ax - (int)c[0]) * (w - 1) / (int)(c[1] - c[0]);
    int pyr = (ay - (int)c[2]) * (h - 1) / (int)(c[3] - c[2]);
    if (invx) pxr = (w - 1) - pxr;
    if (invy) pyr = (h - 1) - pyr;
    ex = abs(pxr - w / 2);
    ey = abs(pyr - h / 2);
    Serial.printf("[cal] pusat meleset x=%d y=%d px (batas %d)\n", ex, ey, w / 20);
    ok = (ex <= w / 20 && ey <= h / 20);
  } else {
    ok = true; /* no centre sample: keep the corner fit and let the user judge */
  }

  if (ok) {
    s_tft->setTextColor(TFT_GREEN, TFT_BLACK);
    s_tft->drawString("KALIBRASI OK", w / 2 - 62, h - 22, 2);
    delay(1200);
  } else {
    s_tft->setTextColor(TFT_RED, TFT_BLACK);
    s_tft->drawString("KALIBRASI KURANG TEPAT", w / 2 - 100, h - 22, 2);
    delay(1600);
  }
  ui_after_calibration();
}
