// =============================================================================
//  CYD touch RETEST  (documented bus only, raw words, ungated sampling)
//
//  Purpose: settle whether the XPT2046 on CLK25/MOSI32/MISO39/CS33 responds to
//  finger contact at all. Every sample logs the full 16-bit word for X, Y and Z
//  together with the IRQ level, so contact and data can be correlated after
//  the fact instead of relying on window timing.
//
//  Decode is shown three ways because a bit-alignment error would hide real
//  movement behind a frozen 4-bit slice:
//     >>3 & 0xFFF   (standard XPT2046 12-bit position)
//     >>4 & 0xFFF   (one bit later - catches a stray leading clock)
//     low 12 bits   (catches a left-shifted frame)
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>

static TFT_eSPI tft = TFT_eSPI();
static SPIClass tp(VSPI);

static const int TP_CLK = 25, TP_MOSI = 32, TP_MISO = 39, TP_CS = 33, TP_IRQ = 36;

// raw words and their observed extremes
static uint16_t wX = 0, wY = 0, wZ = 0;
static uint16_t xMin = 0xFFFF, xMax = 0, yMin = 0xFFFF, yMax = 0, zMin = 0xFFFF, zMax = 0;
static int irqLevel = 1;
static uint32_t contactSamples = 0, totalSamples = 0;

static uint16_t xpt(uint8_t cmd) {
  tp.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TP_CS, LOW);
  uint8_t hi = tp.transfer(cmd);
  uint8_t lo = tp.transfer(0x00);
  digitalWrite(TP_CS, HIGH);
  tp.endTransaction();
  return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static uint16_t decode3(uint16_t w) { return (uint16_t)((w >> 3) & 0x0FFF); }
static uint16_t decode4(uint16_t w) { return (uint16_t)((w >> 4) & 0x0FFF); }
static uint16_t decodeLow(uint16_t w) { return (uint16_t)(w & 0x0FFF); }

static void noteExtremes() {
  if (wX < xMin) xMin = wX;
  if (wX > xMax) xMax = wX;
  if (wY < yMin) yMin = wY;
  if (wY > yMax) yMax = wY;
  if (wZ < zMin) zMin = wZ;
  if (wZ > zMax) zMax = wZ;
  totalSamples++;
}

static void drawStatic() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("TOUCH RETEST  clk25 mosi32 miso39 cs33", 6, 4);

  tft.setTextFont(1);
  tft.setTextColor(0x8410, TFT_BLACK);
  tft.drawString("press and hold the screen - watch the words", 6, 26);
}

static void drawLive() {
  // IRQ + contact counter
  tft.fillRect(0, 42, 320, 22, TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(irqLevel == 0 ? TFT_GREEN : 0x8410, TFT_BLACK);
  tft.drawString(irqLevel == 0 ? "CONTACT" : "idle", 6, 44);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  char b[48];
  snprintf(b, sizeof(b), "samples %lu", (unsigned long)totalSamples);
  tft.drawString(b, 120, 44);

  // raw words
  tft.fillRect(0, 70, 320, 96, TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  snprintf(b, sizeof(b), "X 0x%04X", wX); tft.drawString(b, 6, 70);
  snprintf(b, sizeof(b), "Y 0x%04X", wY); tft.drawString(b, 6, 94);
  snprintf(b, sizeof(b), "Z 0x%04X", wZ); tft.drawString(b, 6, 118);

  tft.setTextFont(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(b, sizeof(b), ">>3 %4u / %4u / %4u",
           decode3(wX), decode3(wY), decode3(wZ));
  tft.drawString(b, 6, 146);
  snprintf(b, sizeof(b), ">>4 %4u / %4u / %4u",
           decode4(wX), decode4(wY), decode4(wZ));
  tft.drawString(b, 6, 158);

  // extremes
  tft.fillRect(0, 176, 320, 60, TFT_BLACK);
  tft.setTextColor(0x8410, TFT_BLACK);
  snprintf(b, sizeof(b), "Xmin/max 0x%04X / 0x%04X", xMin, xMax);
  tft.drawString(b, 6, 178);
  snprintf(b, sizeof(b), "Ymin/max 0x%04X / 0x%04X", yMin, yMax);
  tft.drawString(b, 6, 190);
  snprintf(b, sizeof(b), "Zmin/max 0x%04X / 0x%04X", zMin, zMax);
  tft.drawString(b, 6, 202);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

  pinMode(TP_CS, OUTPUT);
  digitalWrite(TP_CS, HIGH);
  pinMode(TP_IRQ, INPUT);

  tp.begin(TP_CLK, TP_MISO, TP_MOSI, TP_CS);
  digitalWrite(TP_CS, HIGH);

  tft.init();
  tft.setRotation(1);
  drawStatic();

  Serial.println();
  Serial.println("=== CYD TOUCH RETEST (ungated) ===");
  Serial.println("irq Xword Yword Zword | x3 y3 z3 | x4 y4 | low12x");
}

void loop() {
  static uint32_t t = 0;
  if (millis() - t >= 120) {
    t = millis();

    irqLevel = digitalRead(TP_IRQ);

    wX = xpt(0xD0);
    wY = xpt(0x90);
    wZ = xpt(0xB0);
    noteExtremes();
    if (irqLevel == 0) contactSamples++;

    Serial.printf("%d 0x%04X 0x%04X 0x%04X | %4u %4u %4u | %4u %4u | %4u\n",
                  irqLevel, wX, wY, wZ,
                  decode3(wX), decode3(wY), decode3(wZ),
                  decode4(wX), decode4(wY),
                  decodeLow(wX));

    drawLive();
  }
  delay(5);
}
