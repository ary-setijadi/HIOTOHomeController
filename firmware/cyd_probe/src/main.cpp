// =============================================================================
//  CYD Hardware Probe - ESP32-2432S028 (2.8" "Cheap Yellow Display")
//
//  Two jobs:
//    1. Read the display controller ID by talking to the panel directly over
//       SPI. This is ground truth: it does not depend on which driver TFT_eSPI
//       was compiled with, so it settles "ILI9341 or ST7789?" once and for all.
//    2. Exercise and report every on-board peripheral: backlight, touch, SD,
//       RGB LED, LDR, BOOT button, speaker, plus chip/flash/heap identity.
//
//  Everything is printed to Serial @115200 AND drawn on the panel.
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <SD.h>

// ---------------------------------------------------------------------------
//  CYD pin map (ESP32-2432S028, both 2.8" variants)
// ---------------------------------------------------------------------------
static const int PIN_TFT_SCLK = 14;
static const int PIN_TFT_MOSI = 13;
static const int PIN_TFT_MISO = 12;
static const int PIN_TFT_CS   = 15;
static const int PIN_TFT_DC   = 2;
static const int PIN_TFT_BL   = 21;
static const int PIN_TFT_RST  = -1;   // tied to module EN on this board

static const int PIN_TP_SCLK = 25;
static const int PIN_TP_MOSI = 32;
static const int PIN_TP_MISO = 39;
static const int PIN_TP_CS   = 33;
static const int PIN_TP_IRQ  = 36;    // input only

static const int PIN_SD_CS   = 5;
static const int PIN_SD_SCLK = 18;
static const int PIN_SD_MISO = 19;
static const int PIN_SD_MOSI = 23;

static const int PIN_LED_R = 4;       // all three ACTIVE LOW
static const int PIN_LED_G = 16;
static const int PIN_LED_B = 17;

static const int PIN_LDR   = 34;      // input only, ADC1
static const int PIN_SPK   = 26;      // via on-board amp (I2S DAC / DAC2)
static const int PIN_BOOT  = 0;       // BOOT button, active LOW

// ---------------------------------------------------------------------------
//  Report plumbing
// ---------------------------------------------------------------------------
static String g_lines[80];
static int    g_nLines = 0;

static void addLine(const String &s) {
  if (g_nLines < (int)(sizeof(g_lines) / sizeof(g_lines[0]))) g_lines[g_nLines++] = s;
}

static void addKV(const char *k, const String &v) {
  addLine(String("  ") + k + ": " + v);
}

// ---------------------------------------------------------------------------
//  Register-level display ID probe
// ---------------------------------------------------------------------------
static SPIClass dispSpi(HSPI);
static uint32_t g_id04 = 0;   // RDDID (0x04) - ST77xx family answers here
static uint32_t g_idD3 = 0;   // RDID4 (0xD3) - ILI9xxx family answers here
static bool     g_dispResponded = false;

static void rawCmd(uint8_t c) {
  digitalWrite(PIN_TFT_DC, LOW);
  digitalWrite(PIN_TFT_CS, LOW);
  dispSpi.transfer(c);
  digitalWrite(PIN_TFT_CS, HIGH);
}

static uint32_t rawRead(uint8_t cmd, uint8_t nbytes, uint8_t dummy) {
  uint32_t v = 0;
  digitalWrite(PIN_TFT_DC, LOW);
  digitalWrite(PIN_TFT_CS, LOW);
  dispSpi.transfer(cmd);
  digitalWrite(PIN_TFT_DC, HIGH);
  for (uint8_t i = 0; i < dummy; i++) dispSpi.transfer(0x00);
  for (uint8_t i = 0; i < nbytes; i++) v = (v << 8) | dispSpi.transfer(0x00);
  digitalWrite(PIN_TFT_CS, HIGH);
  return v & 0xFFFFFFUL;
}

static String hex24(uint32_t v) {
  char b[10];
  snprintf(b, sizeof(b), "0x%06lX", (unsigned long)v);
  return String(b);
}

static void probeDisplayId() {
  pinMode(PIN_TFT_DC, OUTPUT);
  pinMode(PIN_TFT_CS, OUTPUT);
  digitalWrite(PIN_TFT_CS, HIGH);
  digitalWrite(PIN_TFT_DC, HIGH);

  dispSpi.begin(PIN_TFT_SCLK, PIN_TFT_MISO, PIN_TFT_MOSI, PIN_TFT_CS);

  // Two passes: 1 MHz first (MISO on this board is marginal at higher rates),
  // then 4 MHz only if the first pass produced nothing but zeros.
  for (uint8_t pass = 0; pass < 2; pass++) {
    dispSpi.beginTransaction(
        SPISettings(pass == 0 ? 1000000UL : 4000000UL, MSBFIRST, SPI_MODE0));

    rawCmd(0x01);                     // SWRESET
    delay(150);
    rawCmd(0x11);                     // SLPOUT - reads are unreliable while asleep
    delay(120);

    g_id04 = rawRead(0x04, 3, 1);     // RDDID -> ST7789 = 0x008585
    g_idD3 = rawRead(0xD3, 3, 1);     // RDID4 -> ILI9341 = 0x009341  (NOT 0xDA)

    dispSpi.endTransaction();
    if (g_id04 != 0 || g_idD3 != 0) break;
  }

  dispSpi.end();

  g_dispResponded = !(g_id04 == 0 && g_idD3 == 0);
}

static String classifyController() {
  const uint16_t a = (uint16_t)(g_id04 & 0xFFFF);
  const uint16_t b = (uint16_t)(g_idD3 & 0xFFFF);

  if (b == 0x9341) return "ILI9341";
  if (b == 0x9488) return "ILI9488";
  if (b == 0x9486) return "ILI9486";
  if (a == 0x8585) return "ST7789";
  if (a == 0x8552) return "ST7789 (alt ID)";
  if (a == 0x7796) return "ST7796";
  if (a == 0x7789) return "ST7789 (7789 ID)";
  if (a == 0xFFFF || (a == 0 && b == 0)) return "NO ID RESPONSE";
  return "UNRECOGNISED (MISO noise)";
}

// ---------------------------------------------------------------------------
//  XPT2046 resistive touch (own SPI bus: 25/32/39/33)
// ---------------------------------------------------------------------------
static SPIClass tpSpi(VSPI);
static uint16_t g_tpRawX = 0, g_tpRawY = 0, g_tpZ = 0, g_tpIRQ = 1;
static bool     g_tpOk = false;

static uint16_t xptRead(uint8_t cmd) {
  digitalWrite(PIN_TP_CS, LOW);
  tpSpi.transfer(cmd);                 // 8 clocks: command (not part of the reply)
  uint8_t hi = tpSpi.transfer(0x00);   // 8 clocks: dummy + data bits 11..5
  uint8_t lo = tpSpi.transfer(0x00);   // 8 clocks: data bits 4..0 + zeros
  digitalWrite(PIN_TP_CS, HIGH);
  return (uint16_t)((((uint16_t)hi << 8) | lo) >> 3) & 0x0FFF;
}

static void probeTouch() {
  pinMode(PIN_TP_CS, OUTPUT);
  digitalWrite(PIN_TP_CS, HIGH);
  pinMode(PIN_TP_IRQ, INPUT);

  tpSpi.begin(PIN_TP_SCLK, PIN_TP_MISO, PIN_TP_MOSI, PIN_TP_CS);
  tpSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));

  for (int i = 0; i < 8; i++) {
    g_tpRawX = xptRead(0xD0);   // X position
    g_tpRawY = xptRead(0x90);   // Y position
    g_tpZ   = xptRead(0xB0);    // pressure
    delay(3);
  }
  tpSpi.endTransaction();
  // Deliberately keep the VSPI bus initialised so loop() can keep sampling.

  g_tpIRQ = digitalRead(PIN_TP_IRQ);
  // A live XPT2046 returns a value that is neither 0 nor 4095 when idle.
  g_tpOk = (g_tpRawX > 0 && g_tpRawX < 0x0FFF);
}

// ---------------------------------------------------------------------------
//  microSD (VSPI on 18/19/23, CS 5) - shares VSPI with touch, so run in turn
// ---------------------------------------------------------------------------
static bool     g_sdOk = false;
static uint8_t  g_sdType = 0;
static uint64_t g_sdSizeMB = 0;
static int      g_sdFiles = -1;

static void probeSd() {
  SPIClass sdSpi(VSPI);
  sdSpi.begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

  if (SD.begin(PIN_SD_CS, sdSpi, 10000000)) {
    g_sdOk = true;
    g_sdType = (uint8_t)SD.cardType();
    g_sdSizeMB = SD.cardSize() / (1024ULL * 1024ULL);
    File root = SD.open("/");
    if (root) {
      int n = 0;
      while (n < 999) {
        File f = root.openNextFile();
        if (!f) break;
        n++;
        f.close();
      }
      g_sdFiles = n;
      root.close();
    }
    SD.end();
  }
  sdSpi.end();
}

// ---------------------------------------------------------------------------
//  Screen rendering
// ---------------------------------------------------------------------------
static TFT_eSPI tft = TFT_eSPI();

static void drawReport() {
  tft.init();
  tft.setRotation(1);                    // 320x240 landscape
  tft.fillScreen(TFT_BLACK);

  // Colour bars make an inverted / BGR-swapped panel obvious at a glance.
  const uint16_t bars[8] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE,
                            TFT_YELLOW, TFT_CYAN, TFT_MAGENTA, TFT_ORANGE};
  for (int i = 0; i < 8; i++) tft.fillRect(i * 40, 0, 40, 14, bars[i]);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(2, 20);
  tft.println("CYD HARDWARE PROBE  ESP32-2432S028");
  tft.setCursor(2, 30);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.println("controller: " + classifyController() + "  (ID04 " + hex24(g_id04) +
              "  IDD3 " + hex24(g_idD3) + ")");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  int y = 44;
  for (int i = 0; i < g_nLines && y < 232; i++, y += 9) {
    tft.setCursor(2, y);
    tft.println(g_lines[i]);
  }
}

// ---------------------------------------------------------------------------
//  setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("==============================================");
  Serial.println(" CYD Hardware Probe - ESP32-2432S028");
  Serial.println("==============================================");

  // --- identity -----------------------------------------------------------
  addLine("--- SoC ---");
  addKV("chip", ESP.getChipModel());
  addKV("revision", String('r') + ESP.getChipRevision());
  addKV("cores", String(ESP.getChipCores()));
  addKV("cpu MHz", String(getCpuFrequencyMhz()));
  addKV("flash MB", String(ESP.getFlashChipSize() / (1024 * 1024)));
  addKV("flash speed", String(ESP.getFlashChipSpeed() / 1000000) + " MHz");
  addKV("PSRAM", psramFound() ? String(ESP.getPsramSize() / 1024) + " KB" : "none");
  addKV("free heap", String(ESP.getFreeHeap() / 1024) + " KB");
  addKV("SDK", String(ESP.getSdkVersion()));
  {
    uint64_t mac = ESP.getEfuseMac();          // little-endian: low byte is MAC[0]
    char mb[20];
    snprintf(mb, sizeof(mb), "%02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned)(mac & 0xFF), (unsigned)((mac >> 8) & 0xFF),
             (unsigned)((mac >> 16) & 0xFF), (unsigned)((mac >> 24) & 0xFF),
             (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
    addKV("MAC", String(mb));
  }

  // --- display controller (ground truth) ----------------------------------
  probeDisplayId();
  addLine("--- display ---");
  addKV("controller", classifyController());
  addKV("RDDID 0x04", hex24(g_id04));
  addKV("RDID4 0xD3", hex24(g_idD3));
  addKV("responded", g_dispResponded ? "yes" : "no - check BL/pins");

  // --- SD then touch (both need VSPI, so strictly sequential) -------------
  probeSd();
  addLine("--- microSD ---");
  if (g_sdOk) {
    const char *types[] = {"none", "MMC", "SDSC", "SDHC"};
    addKV("card", (g_sdType <= 3) ? types[g_sdType] : "unknown");
    addKV("size MB", String((unsigned long)g_sdSizeMB));
    addKV("root entries", String(g_sdFiles));
  } else {
    addKV("card", "not detected (empty slot is normal)");
  }

  probeTouch();
  addLine("--- touch (XPT2046) ---");
  addKV("raw X/Y/Z", String(g_tpRawX) + " / " + String(g_tpRawY) + " / " + String(g_tpZ));
  addKV("IRQ (GPIO36)", String(g_tpIRQ) + (g_tpIRQ == 0 ? " (pressed)" : " (idle)"));
  addKV("status", g_tpOk ? "responding" : "no valid reading");

  // --- RGB LED (active low) ----------------------------------------------
  addLine("--- RGB LED (active LOW) ---");
  pinMode(PIN_LED_R, OUTPUT); pinMode(PIN_LED_G, OUTPUT); pinMode(PIN_LED_B, OUTPUT);
  const int ledPin[3] = {PIN_LED_R, PIN_LED_G, PIN_LED_B};
  const uint16_t ledCol[3] = {TFT_RED, TFT_GREEN, TFT_BLUE};
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_LED_R, HIGH); digitalWrite(PIN_LED_G, HIGH); digitalWrite(PIN_LED_B, HIGH);
    digitalWrite(ledPin[i], LOW);          // LOW = ON
    delay(220);
  }
  digitalWrite(PIN_LED_R, HIGH); digitalWrite(PIN_LED_G, HIGH); digitalWrite(PIN_LED_B, HIGH);
  addKV("pins R/G/B", "4 / 16 / 17  (LOW = ON)");
  addKV("test", "cycled red/green/blue - watch the LED");

  // --- LDR --------------------------------------------------------------
  analogSetPinAttenuation(PIN_LDR, ADC_11db);
  int ldr = analogRead(PIN_LDR);
  addLine("--- light sensor ---");
  addKV("GPIO34 raw", String(ldr));

  // --- BOOT button ------------------------------------------------------
  pinMode(PIN_BOOT, INPUT_PULLUP);
  addLine("--- button ---");
  addKV("BOOT GPIO0", String(digitalRead(PIN_BOOT)) + " (0 = pressed)");

  // --- speaker: short DAC ramp on GPIO26 (needs a speaker on P4) ---------
  addLine("--- speaker ---");
  addKV("GPIO26", "I2S DAC / DAC2 via on-board amp");
  for (int i = 0; i < 40; i++) { dacWrite(PIN_SPK, (i % 2) ? 120 : 0); delay(2); }
  dacWrite(PIN_SPK, 0);
  addKV("test", "200 ms square-wave chirp played");

  // --- recommended defines ----------------------------------------------
  addLine("--- recommended TFT_eSPI defines ---");
  const String ctrl = classifyController();
  if (ctrl.startsWith("ILI9341")) {
    addLine("  #define ILI9341_2_DRIVER");
    addLine("  #define TFT_RGB_ORDER TFT_BGR");
  } else if (ctrl.startsWith("ST7789")) {
    addLine("  #define ST7789_DRIVER");
    addLine("  #define TFT_RGB_ORDER TFT_RGB");
  } else {
    addLine("  (controller unresolved - do not trust a driver choice yet)");
  }
  addLine("  #define TFT_WIDTH 240 / TFT_HEIGHT 320");
  addLine("  #define SPI_FREQUENCY 40000000");

  // --- flush ------------------------------------------------------------
  Serial.println();
  for (int i = 0; i < g_nLines; i++) Serial.println(g_lines[i]);
  Serial.println();
  Serial.println("Probe complete.");

  // --- backlight + screen ----------------------------------------------
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);        // backlight ON
  drawReport();
}

void loop() {
  // Keep the report on screen; echo live sensor state once a second so touch,
  // LDR and the BOOT button can be verified from the serial side.
  static uint32_t t = 0;
  if (millis() - t > 1000) {
    t = millis();

    tpSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    uint16_t rx = xptRead(0xD0);
    uint16_t ry = xptRead(0x90);
    uint16_t rz = xptRead(0xB0);
    tpSpi.endTransaction();

    Serial.printf("live: LDR=%4d BOOT=%d IRQ=%d  TP x=%4u y=%4u z=%4u\n",
                  analogRead(PIN_LDR), digitalRead(PIN_BOOT),
                  digitalRead(PIN_TP_IRQ), rx, ry, rz);
  }
  delay(20);
}
