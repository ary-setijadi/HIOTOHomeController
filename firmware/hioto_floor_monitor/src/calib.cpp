/**
 * HIOTO Floor Monitor — touchscreen calibration tool (separate env "calib").
 *
 * Shows four big numbered targets plus a full-width colour bar (to prove the
 * whole panel draws). Tap the targets in order; the raw XPT2046 values are
 * printed and the derived calibration is stored in NVS "hioto"/"tcal", which
 * the main firmware applies automatically on its next boot.
 */
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Preferences.h>

static TFT_eSPI tft;
static Preferences prefs;

#define W 480
#define H 320
static const int IX = 56, IY = 60;
static const int s_pos[4][2] = {{IX, IY}, {W - IX, IY}, {IX, H - IY}, {W - IX, H - IY}};
static const char *s_names[4] = {"1 kiri-atas", "2 kanan-atas", "3 kiri-bawah", "4 kanan-bawah"};

static void draw_target(int i, uint16_t color) {
  int x = s_pos[i][0], y = s_pos[i][1];
  tft.fillRect(x - 24, y - 24, 48, 48, color);
  tft.drawRect(x - 26, y - 26, 52, 52, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(s_names[i], x - 34, y + 30, 2);
}

static bool wait_tap(uint16_t *rx, uint16_t *ry) {
  uint32_t t0 = millis();
  while (millis() - t0 < 120000) {
    if (tft.getTouchRawZ() > 300) {
      uint32_t sx = 0, sy = 0;
      int n = 0;
      while (tft.getTouchRawZ() > 300 && n < 10) {
        uint16_t a, b;
        tft.getTouchRaw(&a, &b);
        sx += a;
        sy += b;
        n++;
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

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== HIOTO touch calibration tool ===");

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  /* full-width colour bar: if the rightmost segments are missing, the panel
   * drawing area is wrong (report it). */
  tft.fillRect(0, 0, 120, 24, TFT_RED);
  tft.fillRect(120, 0, 120, 24, TFT_GREEN);
  tft.fillRect(240, 0, 120, 24, TFT_BLUE);
  tft.fillRect(360, 0, 120, 24, TFT_YELLOW);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("kalibrasi sentuh: ketuk 1-2-3-4", 30, 30, 2);

  for (int i = 0; i < 4; i++) draw_target(i, TFT_CYAN);

  uint16_t raw[4][2];
  for (int i = 0; i < 4; i++) {
    Serial.printf("TAP %d: tap target %s\n", i + 1, s_names[i]);
    if (!wait_tap(&raw[i][0], &raw[i][1])) {
      Serial.println("TIMEOUT");
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString("TIMEOUT - unplug to retry", 150, 160, 4);
      return;
    }
    Serial.printf("RAW %d: %u,%u\n", i + 1, (unsigned)raw[i][0], (unsigned)raw[i][1]);
    tft.fillRect(s_pos[i][0] - 24, s_pos[i][1] - 24, 48, 48, TFT_GREEN);
    delay(200);
  }

  /* Same math as TFT_eSPI calibrateTouch(): 4 raw corner points. */
  uint16_t v[8] = {raw[0][0], raw[0][1], raw[2][0], raw[2][1],
                   raw[1][0], raw[1][1], raw[3][0], raw[3][1]};
  bool rotate = abs((int)v[0] - (int)v[4]) > abs((int)v[1] - (int)v[5]);
  int x0, x1, y0, y1;
  if (rotate) {
    x0 = (v[1] + v[3]) / 2;
    x1 = (v[5] + v[7]) / 2;
    y0 = (v[0] + v[4]) / 2;
    y1 = (v[2] + v[6]) / 2;
  } else {
    x0 = (v[0] + v[2]) / 2;
    x1 = (v[4] + v[6]) / 2;
    y0 = (v[1] + v[5]) / 2;
    y1 = (v[3] + v[7]) / 2;
  }
  bool invx = x0 > x1, invy = y0 > y1;
  if (invx) { int t = x0; x0 = x1; x1 = t; }
  if (invy) { int t = y0; y0 = y1; y1 = t; }

  uint16_t cal[8];
  cal[0] = (uint16_t)x0;
  cal[1] = (uint16_t)(x1 - x0);
  cal[2] = (uint16_t)y0;
  cal[3] = (uint16_t)(y1 - y0);
  cal[4] = (uint16_t)((rotate ? 1 : 0) | (invx ? 2 : 0) | (invy ? 4 : 0));
  cal[5] = cal[6] = cal[7] = 0;

  prefs.begin("hioto", false);
  prefs.putBytes("tcal", cal, sizeof cal);
  prefs.end();

  Serial.printf("CALIB x0=%u x1=%u y0=%u y1=%u flags=%u\n", cal[0], cal[1], cal[2], cal[3],
                cal[4]);
  Serial.println("CALIB saved to NVS. Reflash the main app; it will load this.");

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("KALIBRASI TERSIMPAN", 130, 120, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Reflash main app (hioto_3248s035)", 110, 160, 2);
}

void loop() {
  /* idle; calibration already saved */
  delay(500);
  if (millis() % 5000 < 500) Serial.println("calib tool idle (done)");
}
