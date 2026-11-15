// =============================================================================
//  CYD touch bus scanner
//
//  Question: which pins does the XPT2046 on this specific board actually use?
//
//  The classic 2.8" CYD (PINS.md) puts it on its own bus:
//       CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36
//
//  But this unit is the dual-USB/ST7789 variant, and its shipping firmware
//  assumed touch shared the *display* bus (CS 33 with MOSI 13 / CLK 14 / MISO 12).
//  The probe measured IRQ 36 working while X/Y stayed near zero - which is what
//  you see when you read the wrong pins.
//
//  So the firmware samples BOTH candidates for 30 s and reports which one shows
//  a raw span that grows when you press the screen. During the scan the display
//  is deliberately left un-initialised, because TFT_eSPI owns the display pins
//  through the SPI peripheral and would fight the bit-banged candidate.
//
//  Press the screen repeatedly for the whole 30 s window. The RGB LED glows
//  blue while scanning; afterwards the verdict appears on the panel.
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>

// candidate A - classic CYD touch bus
static const int A_SCLK = 25, A_MOSI = 32, A_MISO = 39;
// candidate B - touch on the display bus
static const int B_SCLK = 14, B_MOSI = 13, B_MISO = 12;
static const int TP_CS  = 33;          // shared by both candidates
static const int TP_IRQ = 36;
static const int BTN    = 0;
static const int LED_R = 4, LED_G = 16, LED_B = 17;

static TFT_eSPI tft = TFT_eSPI();
static SPIClass spiA(VSPI);

struct Probe {
  const char *name = "";
  uint16_t x = 0, y = 0, z = 0;        // 12-bit interpretations
  uint16_t x16 = 0, y16 = 0, z16 = 0;  // raw 16-bit words
  uint16_t xMin = 4095, xMax = 0, yMin = 4095, yMax = 0;

  Probe() {}
  explicit Probe(const char *n) : name(n) {}

  void note() {
    if (x < xMin) xMin = x;
    if (x > xMax) xMax = x;
    if (y < yMin) yMin = y;
    if (y > yMax) yMax = y;
  }
  void reset() { xMin = 4095; xMax = 0; yMin = 4095; yMax = 0; }
  uint16_t spanX() const { return (xMax > xMin) ? (uint16_t)(xMax - xMin) : 0; }
  uint16_t spanY() const { return (yMax > yMin) ? (uint16_t)(yMax - yMin) : 0; }
  uint16_t span()  const { return max(spanX(), spanY()); }
};

static Probe candA("A hw 25/32/39/33");
static Probe candB("B bb 13/14/12 cs33");

// ---------------------------------------------------------------- readers ---
static uint16_t readA(uint8_t cmd) {
  spiA.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TP_CS, LOW);
  uint8_t hi = spiA.transfer(cmd);
  uint8_t lo = spiA.transfer(0x00);
  digitalWrite(TP_CS, HIGH);
  spiA.endTransaction();
  return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static uint16_t readB(uint8_t cmd) {
  uint16_t v = 0;
  digitalWrite(TP_CS, LOW);
  delayMicroseconds(2);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(B_MOSI, (cmd >> i) & 1);
    delayMicroseconds(1);
    digitalWrite(B_SCLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(B_SCLK, LOW);
    delayMicroseconds(1);
  }
  for (int i = 0; i < 16; i++) {
    digitalWrite(B_SCLK, HIGH);
    delayMicroseconds(1);
    v = (uint16_t)((v << 1) | (digitalRead(B_MISO) ? 1 : 0));
    delayMicroseconds(1);
    digitalWrite(B_SCLK, LOW);
    delayMicroseconds(1);
  }
  digitalWrite(TP_CS, HIGH);
  return v;
}

static void sample(Probe &p, uint16_t (*rd)(uint8_t)) {
  p.x16 = rd(0xD0);
  p.y16 = rd(0x90);
  p.z16 = rd(0xB0);
  p.x = (uint16_t)((p.x16 >> 3) & 0x0FFF);
  p.y = (uint16_t)((p.y16 >> 3) & 0x0FFF);
  p.z = (uint16_t)((p.z16 >> 3) & 0x0FFF);
  p.note();
}

static void printLine(const char *tag, const Probe &p) {
  Serial.printf("%s x16=0x%04X y16=0x%04X z16=0x%04X | x=%4u y=%4u z=%4u | span=%4u,%4u\n",
                tag, p.x16, p.y16, p.z16, p.x, p.y, p.z, p.spanX(), p.spanY());
}

// ---------------------------------------------------------------- display ---
static void drawVerdict() {
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("TOUCH BUS SCAN COMPLETE", 6, 6);

  tft.setTextFont(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  int y = 34;
  tft.drawString("candidate              span X / Y", 6, y); y += 14;

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("A  hw 25/32/39/33", 6, y);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String(candA.spanX()) + " / " + String(candA.spanY()), 170, y);
  y += 14;

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("B  bb 13/14/12 cs33", 6, y);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String(candB.spanX()) + " / " + String(candB.spanY()), 170, y);
  y += 22;

  const uint16_t WIN = 120;   // a real axis sweeps hundreds of counts
  const char *verdict;
  uint16_t colour;
  if (candA.span() >= WIN && candA.span() >= candB.span()) {
    verdict = "TOUCH IS ON A: 25/32/39/33";
    colour = TFT_GREEN;
  } else if (candB.span() >= WIN) {
    verdict = "TOUCH IS ON B: display bus";
    colour = TFT_ORANGE;
  } else {
    verdict = "NEITHER: no usable axis";
    colour = TFT_RED;
  }

  tft.setTextFont(2);
  tft.setTextColor(colour, TFT_BLACK);
  tft.drawString(verdict, 6, y); y += 22;

  tft.setTextFont(1);
  tft.setTextColor(0x8410, TFT_BLACK);
  tft.drawString("serial log holds every sample; BOOT restarts the scan.", 6, y); y += 12;
  tft.drawString("live A below (B unreadable once the panel owns the pins)", 6, y);
}

// ---------------------------------------------------------------- lifecycle ---
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); digitalWrite(LED_B, HIGH);
  digitalWrite(LED_B, LOW);                    // blue = scanning

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);                      // backlight on, but nothing drawn

  pinMode(TP_CS, OUTPUT);
  digitalWrite(TP_CS, HIGH);
  pinMode(TP_IRQ, INPUT);
  pinMode(B_SCLK, OUTPUT);
  pinMode(B_MOSI, OUTPUT);
  pinMode(B_MISO, INPUT);                      // GPIO12: no internal pull-up

  spiA.begin(A_SCLK, A_MISO, A_MOSI, TP_CS);
  digitalWrite(TP_CS, HIGH);

  Serial.println();
  Serial.println("=== CYD TOUCH BUS SCAN - press the screen repeatedly now ===");
  Serial.printf("IRQ idle = %d\n", digitalRead(TP_IRQ));

  const uint32_t WINDOW_MS = 30000;
  uint32_t start = millis();
  uint32_t lastPrint = 0;

  while (millis() - start < WINDOW_MS) {
    sample(candA, readA);
    sample(candB, readB);

    if (millis() - lastPrint > 180) {
      lastPrint = millis();
      int irq = digitalRead(TP_IRQ);
      Serial.printf("[%5lu ms] IRQ=%d\n", (unsigned long)(millis() - start), irq);
      printLine("  ", candA);
      printLine("  ", candB);
    }
    delay(20);
  }

  Serial.println("=== scan complete ===");
  printLine("A final", candA);
  printLine("B final", candB);
  Serial.printf("A span=%u  B span=%u  winner=%s\n",
                candA.span(), candB.span(),
                (candA.span() >= candB.span()) ? "A" : "B");

  digitalWrite(LED_B, HIGH);
  drawVerdict();
}

void loop() {
  // keep sampling candidate A so the display stays live; B is meaningless now
  static uint32_t t = 0;
  if (millis() - t > 250) {
    t = millis();
    sample(candA, readA);
    Serial.printf("liveA x=%4u y=%4u z=%4u IRQ=%d span=%u,%u\n",
                  candA.x, candA.y, candA.z, digitalRead(TP_IRQ),
                  candA.spanX(), candA.spanY());
  }
  delay(10);
}
