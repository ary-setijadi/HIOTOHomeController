// =============================================================================
//  CYD touch IDENTIFIER
//
//  Established so far on this unit:
//    - SPI on 25/32/39/33 returns frozen garbage (0x807F) even while pressing
//    - SPI on the display bus returns 0x0000
//    - IRQ on GPIO36 toggles cleanly on finger contact
//
//  That is the fingerprint of a capacitive controller talking I2C, not an
//  XPT2046 talking SPI. This firmware scans I2C across plausible pin pairs,
//  names what it finds, and - if a CST8xx is present - decodes live touch
//  coordinates so the identification is proven, not assumed.
//
//  Build: pio run -e st7789
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <TFT_eSPI.h>

static TFT_eSPI tft = TFT_eSPI();

static const int PIN_IRQ = 36;
static const int LED_R = 4, LED_G = 16, LED_B = 17;

struct Pair { uint8_t sda, scl; const char *label; };
static const Pair PAIRS[] = {
  {33, 32, "sda33 scl32"},
  {32, 33, "sda32 scl33"},
  {22, 27, "sda22 scl27"},
  {27, 22, "sda27 scl22"},
  {25, 32, "sda25 scl32"},
  {32, 25, "sda32 scl25"},
};
static const int NPAIRS = (int)(sizeof(PAIRS) / sizeof(PAIRS[0]));

struct Result {
  const char *label = "";
  uint8_t addrs[6] = {0};
  int n = 0;
};
static Result g_res[NPAIRS];

// set when a CST8xx answers on a specific pair
static int16_t g_cstSda = -1, g_cstScl = -1;

static const char *nameFor(uint8_t a) {
  switch (a) {
    case 0x15: return "CST816/CST820 touch";
    case 0x14: return "GT911 touch (alt)";
    case 0x5D: return "GT911 touch";
    case 0x38: return "FT6236 touch";
    case 0x51: return "PCF8563 RTC";
    case 0x68: return "MPU6050 / DS3231";
    case 0x76: return "BME280 / BMP280";
    case 0x3C: return "SSD1306 OLED";
    default:   return "-";
  }
}

static void scanPair(int idx) {
  g_res[idx].label = PAIRS[idx].label;
  g_res[idx].n = 0;

  Wire.begin(PAIRS[idx].sda, PAIRS[idx].scl, 100000);
  delay(4);
  for (uint8_t a = 0x08; a <= 0x77 && g_res[idx].n < 6; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) g_res[idx].addrs[g_res[idx].n++] = a;
  }
  Wire.end();

  for (int i = 0; i < g_res[idx].n; i++) {
    if (g_res[idx].addrs[i] == 0x15 && g_cstSda < 0) {
      g_cstSda = PAIRS[idx].sda;
      g_cstScl = PAIRS[idx].scl;
    }
  }
}

// CST816/CST820: 0x02 = touch points, 0x03..0x06 = X/Y (12-bit each)
static bool cstPoll(uint8_t &points, uint16_t &x, uint16_t &y) {
  if (g_cstSda < 0) return false;

  Wire.begin((uint8_t)g_cstSda, (uint8_t)g_cstScl, 100000);
  Wire.beginTransmission(0x15);
  Wire.write((uint8_t)0x02);
  if (Wire.endTransmission(false) != 0) { Wire.end(); return false; }
  if (Wire.requestFrom((uint8_t)0x15, (uint8_t)5) != 5) { Wire.end(); return false; }

  points = Wire.read();
  uint8_t xh = Wire.read(), xl = Wire.read();
  uint8_t yh = Wire.read(), yl = Wire.read();
  x = (uint16_t)(((xh & 0x0F) << 8) | xl);
  y = (uint16_t)(((yh & 0x0F) << 8) | yl);
  Wire.end();
  return true;
}

static void drawSummary() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("TOUCH IDENTIFIER", 6, 4);

  tft.setTextFont(1);
  int y = 28;
  bool any = false;
  for (int i = 0; i < NPAIRS; i++) {
    if (g_res[i].n == 0) continue;
    any = true;
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(g_res[i].label, 6, y);
    y += 11;
    for (int k = 0; k < g_res[i].n; k++) {
      char b[48];
      snprintf(b, sizeof(b), "   0x%02X  %s", g_res[i].addrs[k],
               nameFor(g_res[i].addrs[k]));
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString(b, 6, y);
      y += 11;
      if (y > 170) break;
    }
    if (y > 170) break;
  }
  if (!any) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("no I2C device on any pair", 6, y);
    y += 12;
  }

  y = 150;
  tft.setTextFont(2);
  if (g_cstSda >= 0) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("CST8xx FOUND - capacitive", 6, y);
  } else {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("no CST/GT device found", 6, y);
  }

  tft.setTextFont(1);
  tft.setTextColor(0x8410, TFT_BLACK);
  tft.drawString("touch the screen: live decode below", 6, 176);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); digitalWrite(LED_B, HIGH);

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  pinMode(PIN_IRQ, INPUT);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("scanning I2C...", 6, 4);

  Serial.println();
  Serial.println("=== CYD TOUCH IDENTIFIER: I2C scan ===");
  Serial.printf("IRQ(GPIO36) idle = %d\n", digitalRead(PIN_IRQ));
  Serial.printf("pin levels: 25=%d 26=%d 32=%d 33=%d 39=%d 36=%d\n",
                digitalRead(25), digitalRead(26), digitalRead(32),
                digitalRead(33), digitalRead(39), digitalRead(36));

  for (int i = 0; i < NPAIRS; i++) {
    scanPair(i);
    if (g_res[i].n == 0) {
      Serial.printf("[%s] none\n", g_res[i].label);
    } else {
      for (int k = 0; k < g_res[i].n; k++) {
        Serial.printf("[%s] 0x%02X  %s\n", g_res[i].label,
                      g_res[i].addrs[k], nameFor(g_res[i].addrs[k]));
      }
    }
  }

  if (g_cstSda >= 0) {
    Serial.printf(">>> CST8xx at 0x15 on sda=%d scl=%d (capacitive touch)\n",
                  g_cstSda, g_cstScl);
    uint8_t pts; uint16_t x, y;
    if (cstPoll(pts, x, y)) {
      Serial.printf(">>> first read: points=%u x=%u y=%u\n", pts, x, y);
    }
  } else {
    Serial.println(">>> no capacitive controller found at 0x15");
  }

  drawSummary();
  digitalWrite(LED_G, LOW);      // green = scan finished
}

void loop() {
  static uint32_t t = 0;
  static int lastIrq = -1;

  int irq = digitalRead(PIN_IRQ);
  if (irq != lastIrq) {
    lastIrq = irq;
    Serial.printf("[irq] GPIO36 -> %d%s\n", irq, irq == 0 ? "  (contact)" : "");
  }

  if (millis() - t > 150) {
    t = millis();
    uint8_t pts = 0; uint16_t x = 0, y = 0;
    if (cstPoll(pts, x, y)) {
      Serial.printf("cst points=%u x=%4u y=%4u irq=%d\n", pts, x, y, irq);

      tft.fillRect(0, 186, 320, 54, TFT_BLACK);
      tft.setTextDatum(TL_DATUM);
      tft.setTextFont(2);
      tft.setTextColor(pts ? TFT_GREEN : 0x8410, TFT_BLACK);
      char b[48];
      snprintf(b, sizeof(b), "pts=%u", pts);
      tft.drawString(b, 6, 188);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      snprintf(b, sizeof(b), "x=%u y=%u", x, y);
      tft.drawString(b, 90, 188);
      tft.fillRect(6, 210, 308, 20, 0x10A2);
      if (pts) {
        int px = 6 + (int)((long)x * 307 / 4095);
        int py = 210 + (int)((long)y * 19 / 4095);
        tft.fillCircle(px, py, 4, TFT_GREEN);
      }
    } else {
      static bool warned = false;
      if (!warned) { warned = true; Serial.println("cst read failed"); }
    }
  }

  delay(10);
}
