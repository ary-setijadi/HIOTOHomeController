// =============================================================================
//  CYD Findings Demo UI - ESP32-2432S028 (2.8" "Cheap Yellow Display")
//
//  Shows, on the panel itself, what was actually measured on this unit:
//    - the real controller is ST7789, not ILI9341
//    - what the wrong driver looked like (noise) vs what the panel can do
//    - SoC / flash / memory identity
//    - the complete pin map
//    - live XPT2046 touch telemetry (IRQ + raw axes)
//    - the exact config diff that caused the garbage
//    - LDR / BOOT / RGB LED / microSD checks
//
//  Navigation: BOOT button (GPIO0) = next screen, wrapping.
//              Touch prev/next buttons in the footer, once a raw range has
//              been learned by tapping the corners of the screen.
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <SD.h>

// ---------------------------------------------------------------- pin map ---
static const int TP_SCLK = 25, TP_MOSI = 32, TP_MISO = 39, TP_CS = 33, TP_IRQ = 36;
static const int SD_CS = 5, SD_SCLK = 18, SD_MISO = 19, SD_MOSI = 23;
static const int LED_R = 4, LED_G = 16, LED_B = 17;      // active LOW
static const int PIN_LDR = 34, PIN_BTN = 0;

// ---------------------------------------------------------------- palette ---
#define C_BG    0x0000
#define C_CARD  0x10A2
#define C_DIM   0x8410
#define C_ACC   TFT_GREEN
#define C_WARN  TFT_ORANGE
#define C_TXT   TFT_WHITE
#define C_BAD   TFT_RED

static TFT_eSPI tft = TFT_eSPI();
static SPIClass tp(VSPI);

static bool    g_sdOk = false;
static uint8_t g_sdType = 0;

// ------------------------------------------------------------------ touch ---
struct TouchState {
  uint16_t x = 0, y = 0, z = 0;
  bool     down = false;
  uint16_t xMin = 4095, xMax = 0, yMin = 4095, yMax = 0;
  bool haveRange() const {
    return (uint16_t)(xMax - xMin) > 120 && (uint16_t)(yMax - yMin) > 120;
  }
} ts;

// XPT2046 frame: 8 clocks of command, then 16 clocks of reply
// ([dummy][12 data bits][3 zeros]). The byte clocked DURING the command is not
// part of the reply - reading only one byte after it truncates the value to its
// top 7 bits, which reads as a tiny, nearly-constant number.
static uint16_t xptRead(uint8_t cmd) {
  digitalWrite(TP_CS, LOW);
  tp.transfer(cmd);                 // command
  uint8_t hi = tp.transfer(0x00);   // dummy + data bits 11..5
  uint8_t lo = tp.transfer(0x00);   // data bits 4..0 + trailing zeros
  digitalWrite(TP_CS, HIGH);
  return (uint16_t)((((uint16_t)hi << 8) | lo) >> 3) & 0x0FFF;
}

static void touchPoll() {
  tp.beginTransaction(SPISettings(500000, MSBFIRST, SPI_MODE0));
  ts.down = (digitalRead(TP_IRQ) == LOW);
  ts.x = xptRead(0xD0);          // X  (12-bit differential)
  ts.y = xptRead(0x90);          // Y
  ts.z = xptRead(0xB0);          // pressure
  tp.endTransaction();

  if (ts.down) {
    if (ts.x < ts.xMin) ts.xMin = ts.x;
    if (ts.x > ts.xMax) ts.xMax = ts.x;
    if (ts.y < ts.yMin) ts.yMin = ts.y;
    if (ts.y > ts.yMax) ts.yMax = ts.y;
  }
}

static int scaleAxis(uint16_t v, uint16_t lo, uint16_t hi, int outMax) {
  if (hi <= lo) return 0;
  long num = (long)((int)v - (int)lo) * outMax;
  long den = (long)((int)hi - (int)lo);
  long r = num / den;
  if (r < 0) r = 0;
  if (r > outMax) r = outMax;
  return (int)r;
}

static int touchScreenX() { return scaleAxis(ts.x, ts.xMin, ts.xMax, 319); }
static int touchScreenY() { return scaleAxis(ts.y, ts.yMin, ts.yMax, 239); }

// -------------------------------------------------------------- UI helpers ---
static int g_screen = 0;
static const int SCREENS = 6;
static void drawScreen(int s);

static void header(const char *title) {
  tft.fillRect(0, 0, 320, 24, C_CARD);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_ACC, C_CARD);
  tft.drawString(title, 6, 4);

  char b[12];
  snprintf(b, sizeof(b), "%d/%d", g_screen + 1, SCREENS);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_DIM, C_CARD);
  tft.drawString(b, 314, 4);
}

static void footer(const char *hint) {
  tft.fillRect(0, 216, 320, 24, C_CARD);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(C_DIM, C_CARD);
  tft.drawString(hint, 6, 222);

  tft.drawRect(236, 218, 38, 19, C_ACC);
  tft.drawRect(278, 218, 38, 19, C_ACC);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_ACC, C_CARD);
  tft.drawString("<", 255, 227);
  tft.drawString(">", 297, 227);
}

static void drawNoise(int x, int y, int w, int h) {
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      int r = random(0, 100);
      uint16_t c;
      if (r < 40)      c = TFT_BLACK;
      else if (r < 60) c = TFT_WHITE;
      else             c = tft.color565(random(0, 256), random(0, 256), random(0, 256));
      tft.drawPixel(x + i, y + j, c);
    }
  }
}

// ------------------------------------------------------------------ screens ---
static void drawHero() {
  tft.fillScreen(C_BG);
  header("CYD 2.8\" FINDINGS");

  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(C_ACC, C_BG);
  tft.drawString("ST7789", 160, 30);

  tft.setTextFont(2);
  tft.setTextColor(C_BAD, C_BG);
  tft.drawString("NOT ILI9341", 160, 62);

  tft.setTextFont(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("your firmware drove this panel as ILI9341", 160, 84);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_BAD, C_BG);
  tft.drawString("what you saw", 8, 100);
  drawNoise(8, 110, 146, 56);

  tft.setTextColor(C_ACC, C_BG);
  tft.drawString("what it can do", 166, 100);
  const uint16_t bars[6] = {TFT_RED, TFT_GREEN, TFT_BLUE,
                            TFT_YELLOW, TFT_CYAN, TFT_MAGENTA};
  for (int i = 0; i < 6; i++)
    tft.fillRect(166 + i * 24, 110, 24, 56, bars[i]);

  tft.setTextColor(C_TXT, C_BG);
  tft.drawString("same panel - one driver macro apart", 8, 178);
  tft.setTextFont(2);
  tft.setTextColor(C_WARN, C_BG);
  tft.drawString("tap corners: teach the touch range", 8, 192);

  footer("BOOT = next screen");
}

static void kv(int x, int y, const char *k, const String &v) {
  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(k, x, y);
  tft.setTextColor(C_TXT, C_BG);
  tft.drawString(v, x + 96, y);
}

static void drawSoc() {
  tft.fillScreen(C_BG);
  header("SOC / MEMORY");

  uint64_t mac = ESP.getEfuseMac();
  char mb[20];
  snprintf(mb, sizeof(mb), "%02X:%02X:%02X:%02X:%02X:%02X",
           (unsigned)(mac & 0xFF), (unsigned)((mac >> 8) & 0xFF),
           (unsigned)((mac >> 16) & 0xFF), (unsigned)((mac >> 24) & 0xFF),
           (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));

  int y = 30;
  kv(6, y, "chip", ESP.getChipModel());                     y += 20;
  kv(6, y, "revision", String('v') + ESP.getChipRevision()); y += 20;
  kv(6, y, "flash", String(ESP.getFlashChipSize() / 1048576) + " MB"); y += 20;
  kv(6, y, "PSRAM", psramFound() ? "present" : "none");     y += 20;
  kv(6, y, "SDK", ESP.getSdkVersion());                     y += 20;
  kv(6, y, "MAC", String(mb));                              y += 20;
  kv(6, y, "heap", String(ESP.getFreeHeap() / 1024) + " KB"); y += 26;

  tft.setTextFont(1);
  tft.setTextColor(C_WARN, C_BG);
  tft.drawString("ID read (0x04 / 0xD3) returns noise on this unit,", 6, y);
  y += 12;
  tft.drawString("so the driver test - not the register - identified it.", 6, y);

  footer("BOOT = next screen");
}

static void pinRow(int y, const char *name, int gpio, const char *note) {
  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_TXT, C_BG);
  tft.drawString(name, 8, y);
  tft.setTextColor(C_ACC, C_BG);
  char g[6];
  snprintf(g, sizeof(g), "%d", gpio);
  tft.drawString(g, 132, y);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(note, 168, y);
}

static void drawPins() {
  tft.fillScreen(C_BG);
  header("PIN MAP");
  int y = 28;
  pinRow(y, "TFT MOSI", 13, "HSPI");            y += 12;
  pinRow(y, "TFT SCLK", 14, "HSPI");            y += 12;
  pinRow(y, "TFT MISO", 12, "reads: noisy");    y += 12;
  pinRow(y, "TFT CS",   15, "strapping");       y += 12;
  pinRow(y, "TFT DC",    2, "strapping");       y += 12;
  pinRow(y, "BACKLIGHT",21, "HIGH = on");       y += 12;
  pinRow(y, "TP CLK",   25, "own bus");         y += 12;
  pinRow(y, "TP MOSI",  32, "own bus");         y += 12;
  pinRow(y, "TP MISO",  39, "own bus");         y += 12;
  pinRow(y, "TP CS",    33, "own bus");         y += 12;
  pinRow(y, "TP IRQ",   36, "LOW = touch");     y += 12;
  pinRow(y, "SD CS",     5, "VSPI");            y += 12;
  pinRow(y, "LED R/G/B", 4, "16 / 17 LOW=on");  y += 12;
  pinRow(y, "LDR",      34, "ADC1");            y += 12;
  pinRow(y, "SPEAKER",  26, "via amp");         y += 12;
  footer("BOOT = next screen");
}

static void drawTouchStatic() {
  tft.fillScreen(C_BG);
  header("TOUCH - XPT2046");
  tft.drawRect(6, 30, 308, 118, C_DIM);
  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("tap around the box to learn raw min/max", 8, 32);
  footer("BOOT = next   |   tap <> to page");
}

static void touchDynamic() {
  // live numbers
  tft.fillRect(0, 152, 320, 62, C_BG);
  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(ts.down ? C_ACC : C_DIM, C_BG);

  char b[64];
  snprintf(b, sizeof(b), "x=%4u  y=%4u  z=%4u", ts.x, ts.y, ts.z);
  tft.drawString(b, 8, 152);

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(ts.down ? C_ACC : C_DIM, C_BG);
  tft.drawString(ts.down ? "DOWN" : "idle", 312, 152);

  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(C_DIM, C_BG);
  snprintf(b, sizeof(b), "range x %u..%u   y %u..%u",
           ts.xMin, ts.xMax, ts.yMin, ts.yMax);
  tft.drawString(b, 8, 176);
  tft.drawString(ts.haveRange() ? "mapping: usable"
                                : "mapping: tap two opposite corners",
                 8, 190);

  // crosshair inside the box
  tft.fillRect(8, 44, 304, 102, C_BG);
  tft.drawRect(6, 30, 308, 118, C_DIM);
  if (ts.down) {
    int px = touchScreenX();
    int py = touchScreenY();
    if (px < 8) px = 8;
    if (px > 310) px = 310;
    if (py < 44) py = 44;
    if (py > 144) py = 144;
    tft.drawFastHLine(8, py, 304, C_ACC);
    tft.drawFastVLine(px, 44, 102, C_ACC);
    tft.fillCircle(px, py, 4, C_ACC);
  }
}

static void codeLine(int y, const char *text, uint16_t colour) {
  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(colour, C_BG);
  tft.drawString(text, 8, y);
}

static void drawFix() {
  tft.fillScreen(C_BG);
  header("WHY IT WAS GARBAGE");
  int y = 28;
  codeLine(y, "hioto_general_monitor / hioto_genmon28", C_DIM); y += 14;
  codeLine(y, "", C_TXT);                                      y += 4;
  codeLine(y, "WRONG (garbage / blank):", C_BAD);              y += 13;
  codeLine(y, "  -DILI9341_DRIVER=1", C_BAD);                  y += 12;
  codeLine(y, "  -DTFT_RGB_ORDER=TFT_BGR", C_BAD);             y += 12;
  codeLine(y, "  -DTOUCH_CS=33   (display bus!)", C_BAD);      y += 16;
  codeLine(y, "RIGHT (this panel):", C_ACC);                   y += 13;
  codeLine(y, "  -DST7789_DRIVER=1", C_ACC);                   y += 12;
  codeLine(y, "  -DTFT_RGB_ORDER=TFT_RGB", C_ACC);             y += 12;
  codeLine(y, "  touch on 25/32/39/33, IRQ 36", C_ACC);        y += 16;
  codeLine(y, "TOUCH_CS only works when the XPT2046 shares the", C_DIM); y += 11;
  codeLine(y, "display bus. On the 2.8\" CYD it does NOT - that is", C_DIM); y += 11;
  codeLine(y, "why touch never calibrated (tcal NOT_FOUND).", C_DIM);
  footer("BOOT = next screen");
}

static void drawSensors() {
  tft.fillScreen(C_BG);
  header("PERIPHERALS");

  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("LDR (GPIO34)", 8, 30);
  tft.drawRect(8, 50, 304, 20, C_DIM);

  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("RGB LED", 8, 84);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("SD card", 8, 132);
  {
    static const char *types[] = {"none", "MMC", "SDSC", "SDHC"};
    String sdTxt = g_sdOk ? String("detected, ") +
                                String((g_sdType <= 3) ? types[g_sdType] : "card")
                          : String("empty slot");
    tft.setTextColor(g_sdOk ? C_ACC : C_DIM, C_BG);
    tft.drawString(sdTxt, 100, 132);
  }
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("BOOT", 8, 158);

  tft.setTextFont(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("active LOW: colour steps on this screen", 8, 104);

  footer("BOOT = next screen");
}

static void sensorsDynamic() {
  static uint32_t tSensor = 0, tLed = 0;
  static int phase = 0;

  uint32_t now = millis();

  if (now - tSensor > 200) {
    tSensor = now;
    int ldr = analogRead(PIN_LDR);

    tft.fillRect(9, 51, 302, 18, C_BG);
    int w = scaleAxis((uint16_t)ldr, 0, 4095, 302);
    if (w > 0) tft.fillRect(9, 51, w, 18, C_WARN);

    tft.fillRect(96, 30, 220, 18, C_BG);
    tft.setTextFont(2);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_TXT, C_BG);
    tft.drawString("raw " + String(ldr), 100, 30);

    tft.fillRect(96, 158, 220, 18, C_BG);
    tft.setTextColor(digitalRead(PIN_BTN) == LOW ? C_ACC : C_DIM, C_BG);
    tft.drawString(digitalRead(PIN_BTN) == LOW ? "pressed" : "released", 100, 158);
  }

  if (now - tLed > 500) {
    tLed = now;
    phase = (phase + 1) % 3;
    digitalWrite(LED_R, HIGH);
    digitalWrite(LED_G, HIGH);
    digitalWrite(LED_B, HIGH);
    const int pin[3] = {LED_R, LED_G, LED_B};
    digitalWrite(pin[phase], LOW);          // LOW = ON

    const uint16_t col[3] = {TFT_RED, TFT_GREEN, TFT_BLUE};
    for (int i = 0; i < 3; i++) {
      tft.fillCircle(110 + i * 26, 93, 10, (i == phase) ? col[i] : C_CARD);
    }
  }
}

// ------------------------------------------------------------ screen router ---
static void drawScreen(int s) {
  g_screen = s;
  switch (s) {
    case 0: drawHero();        break;
    case 1: drawSoc();         break;
    case 2: drawPins();        break;
    case 3: drawTouchStatic(); break;
    case 4: drawFix();         break;
    case 5: drawSensors();     break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, HIGH);

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);                 // backlight on

  pinMode(PIN_BTN, INPUT_PULLUP);
  analogSetPinAttenuation(PIN_LDR, ADC_11db);

  tft.init();
  tft.setRotation(1);                     // 320x240 landscape
  tft.fillScreen(C_BG);

  // microSD first: probing it rebinds VSPI, so binding touch first and again
  // afterwards would double-register the APB frequency callback and log
  // "addApbChangeCallback(): duplicate func". Bind the bus exactly once.
  {
    SPIClass sd(VSPI);
    sd.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    if (SD.begin(SD_CS, sd, 10000000)) {
      g_sdOk = true;
      g_sdType = (uint8_t)SD.cardType();
      SD.end();
    }
    sd.end();
  }

  // touch on its own bus - this is the key fix, and it must be bound last
  pinMode(TP_CS, OUTPUT);
  digitalWrite(TP_CS, HIGH);
  pinMode(TP_IRQ, INPUT);
  tp.begin(TP_SCLK, TP_MISO, TP_MOSI, TP_CS);

  Serial.println("CYD findings demo up");
  drawScreen(0);
}

void loop() {
  touchPoll();

  // ---- BOOT button: edge-triggered page advance (debounced) ----
  static bool lastBtn = HIGH;
  static uint32_t lastBtnMs = 0;
  bool btn = digitalRead(PIN_BTN);
  if (lastBtn == HIGH && btn == LOW && millis() - lastBtnMs > 220) {
    lastBtnMs = millis();
    drawScreen((g_screen + 1) % SCREENS);
  }
  lastBtn = btn;

  // ---- footer prev/next buttons, only once the raw range is usable ----
  static bool lastDown = false;
  if (ts.down && !lastDown && ts.haveRange()) {
    int px = touchScreenX(), py = touchScreenY();
    if (py > 214) {
      if (px > 232 && px < 278) {
        drawScreen((g_screen + SCREENS - 1) % SCREENS);
      } else if (px >= 278 && px < 320) {
        drawScreen((g_screen + 1) % SCREENS);
      }
    }
  }
  lastDown = ts.down;

  // ---- per-screen dynamic content ----
  if (g_screen == 3) touchDynamic();
  if (g_screen == 5) sensorsDynamic();

  // echo touch telemetry so the corrected decode can be verified from serial
  static uint32_t tTp = 0;
  if (millis() - tTp > 400) {
    tTp = millis();
    Serial.printf("tp irq=%d x=%4u y=%4u z=%4u range x %u..%u y %u..%u\n",
                  digitalRead(TP_IRQ), ts.x, ts.y, ts.z,
                  ts.xMin, ts.xMax, ts.yMin, ts.yMax);
  }

  delay(15);
}
