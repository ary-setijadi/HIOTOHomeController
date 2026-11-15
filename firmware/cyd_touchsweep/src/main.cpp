// =============================================================================
//  CYD touch pin SWEEP
//
//  Tries every plausible chip-select pin against both candidate SPI wirings,
//  for 25 s, while you press the screen. Results are ranked by how much the
//  decoded X/Y span grows - a real touch controller produces a span of
//  hundreds; a dead line produces ~0.
//
//  Bus A (documented CYD touch bus): CLK 25, MOSI 32, MISO 39
//  Bus B (display bus)            : CLK 14, MOSI 13, MISO 12
//
//  Both are bit-banged so nothing else owns the pins during the sweep.
// =============================================================================

#include <Arduino.h>
#include <TFT_eSPI.h>

static TFT_eSPI tft = TFT_eSPI();

static const int PIN_IRQ = 36;

// CS candidates: output-capable pins that are not part of either bus
static const int CS_PINS[] = {33, 32, 26, 27, 22, 5, 15, 2, 4, 16, 17, 19, 23, 18, 0};
static const int NCS = (int)(sizeof(CS_PINS) / sizeof(CS_PINS[0]));

// pins the user must never be driven here
static const int BUS_A_CLK = 25, BUS_A_MOSI = 32, BUS_A_MISO = 39;
static const int BUS_B_CLK = 14, BUS_B_MOSI = 13, BUS_B_MISO = 12;

struct Combo {
  uint8_t bus;      // 0 = A, 1 = B
  uint8_t cs;
  uint16_t xMin = 4095, xMax = 0, yMin = 4095, yMax = 0;
  bool used = false;

  uint16_t spanX() const { return (xMax > xMin) ? (uint16_t)(xMax - xMin) : 0; }
  uint16_t spanY() const { return (yMax > yMin) ? (uint16_t)(yMax - yMin) : 0; }
  uint16_t score() const { return (uint16_t)(spanX() + spanY()); }
};

static Combo combos[2 * NCS];
static int nCombos = 0;

static void buildCombos() {
  for (int i = 0; i < NCS; i++) {
    int cs = CS_PINS[i];
    if (cs == BUS_A_CLK || cs == BUS_A_MOSI || cs == BUS_A_MISO) continue;
    combos[nCombos].bus = 0;
    combos[nCombos].cs = (uint8_t)cs;
    nCombos++;
  }
  for (int i = 0; i < NCS; i++) {
    int cs = CS_PINS[i];
    if (cs == BUS_B_CLK || cs == BUS_B_MOSI || cs == BUS_B_MISO) continue;
    combos[nCombos].bus = 1;
    combos[nCombos].cs = (uint8_t)cs;
    nCombos++;
  }
}

static void setupBus(uint8_t bus) {
  if (bus == 0) {
    pinMode(BUS_A_CLK, OUTPUT);
    pinMode(BUS_A_MOSI, OUTPUT);
    pinMode(BUS_A_MISO, INPUT);
  } else {
    pinMode(BUS_B_CLK, OUTPUT);
    pinMode(BUS_B_MOSI, OUTPUT);
    pinMode(BUS_B_MISO, INPUT);
  }
}

static uint16_t bbRead(uint8_t bus, uint8_t csPin, uint8_t cmd) {
  const int clk = (bus == 0) ? BUS_A_CLK : BUS_B_CLK;
  const int mosi = (bus == 0) ? BUS_A_MOSI : BUS_B_MOSI;
  const int miso = (bus == 0) ? BUS_A_MISO : BUS_B_MISO;

  pinMode(csPin, OUTPUT);
  digitalWrite(csPin, HIGH);
  digitalWrite(clk, LOW);

  digitalWrite(csPin, LOW);
  delayMicroseconds(2);

  for (int i = 7; i >= 0; i--) {
    digitalWrite(mosi, (cmd >> i) & 1);
    delayMicroseconds(1);
    digitalWrite(clk, HIGH);
    delayMicroseconds(1);
    digitalWrite(clk, LOW);
    delayMicroseconds(1);
  }

  uint16_t v = 0;
  for (int i = 0; i < 16; i++) {
    digitalWrite(clk, HIGH);
    delayMicroseconds(1);
    v = (uint16_t)((v << 1) | (digitalRead(miso) ? 1 : 0));
    delayMicroseconds(1);
    digitalWrite(clk, LOW);
    delayMicroseconds(1);
  }

  digitalWrite(csPin, HIGH);
  return v;
}

static void sampleCombo(Combo &c) {
  const uint16_t x = (uint16_t)((bbRead(c.bus, c.cs, 0xD0) >> 3) & 0x0FFF);
  const uint16_t y = (uint16_t)((bbRead(c.bus, c.cs, 0x90) >> 3) & 0x0FFF);

  if (x < c.xMin) c.xMin = x;
  if (x > c.xMax) c.xMax = x;
  if (y < c.yMin) c.yMin = y;
  if (y > c.yMax) c.yMax = y;
}

static void drawResult(int bestIdx, const char *verdict, uint16_t colour) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("TOUCH PIN SWEEP DONE", 6, 4);

  tft.setTextFont(1);
  int y = 30;
  tft.setTextColor(0x8410, TFT_BLACK);
  tft.drawString("bus  cs   spanX spanY", 6, y);
  y += 12;

  // print the five best-scoring combos
  for (int rank = 0; rank < 5; rank++) {
    int best = -1;
    for (int i = 0; i < nCombos; i++) {
      if (combos[i].used) continue;
      if (best < 0 || combos[i].score() > combos[best].score()) best = i;
    }
    if (best < 0) break;
    combos[best].used = true;

    char b[40];
    snprintf(b, sizeof(b), " %c   %2u   %5u %5u",
             combos[best].bus == 0 ? 'A' : 'B', combos[best].cs,
             combos[best].spanX(), combos[best].spanY());
    tft.setTextColor(rank == 0 ? TFT_WHITE : 0x8410, TFT_BLACK);
    tft.drawString(b, 6, y);
    y += 12;
  }

  tft.setTextFont(2);
  tft.setTextColor(colour, TFT_BLACK);
  tft.drawString(verdict, 6, 168);

  tft.setTextFont(1);
  tft.setTextColor(0x8410, TFT_BLACK);
  tft.drawString("A = clk25 mosi32 miso39    B = clk14 mosi13 miso12", 6, 196);
  tft.drawString("serial log lists every combo", 6, 208);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);          // backlight on, nothing drawn yet

  pinMode(PIN_IRQ, INPUT);

  buildCombos();

  Serial.println();
  Serial.println("=== CYD TOUCH PIN SWEEP ===");
  Serial.printf("combos=%d  window=25s  keep pressing the screen\n", nCombos);
  Serial.println("bus  cs    idleX idleY");

  // --- sweep window ---
  const uint32_t WINDOW = 25000;
  uint32_t start = millis();
  uint32_t lastIrqLog = 0;

  while (millis() - start < WINDOW) {
    setupBus(0);
    for (int i = 0; i < nCombos; i++) {
      if (combos[i].bus != 0) continue;
      sampleCombo(combos[i]);
    }
    setupBus(1);
    for (int i = 0; i < nCombos; i++) {
      if (combos[i].bus != 1) continue;
      sampleCombo(combos[i]);
    }
  }

  // --- report ---
  int bestIdx = -1;
  for (int i = 0; i < nCombos; i++) {
    if (bestIdx < 0 || combos[i].score() > combos[bestIdx].score()) bestIdx = i;
  }

  Serial.println("--- results (sorted by span sum) ---");
  for (int rank = 0; rank < nCombos; rank++) {
    int best = -1;
    for (int i = 0; i < nCombos; i++) {
      if (combos[i].used) continue;
      if (best < 0 || combos[i].score() > combos[best].score()) best = i;
    }
    if (best < 0) break;
    combos[best].used = true;
    Serial.printf("%c  cs=%2u  spanX=%4u spanY=%4u  score=%4u\n",
                  combos[best].bus == 0 ? 'A' : 'B', combos[best].cs,
                  combos[best].spanX(), combos[best].spanY(),
                  combos[best].score());
  }

  uint16_t bestScore = (bestIdx >= 0) ? combos[bestIdx].score() : 0;
  const char *verdict;
  uint16_t colour;
  if (bestScore > 300) {
    verdict = "CANDIDATE FOUND - see bus/cs";
    colour = TFT_GREEN;
  } else if (bestScore > 60) {
    verdict = "WEAK - partial wiring?";
    colour = TFT_ORANGE;
  } else {
    verdict = "NO WORKING CS - data line dead";
    colour = TFT_RED;
  }
  Serial.printf("BEST: bus=%c cs=%u score=%u -> %s\n",
                combos[bestIdx].bus == 0 ? 'A' : 'B', combos[bestIdx].cs,
                bestScore, verdict);

  // reset used flags so the renderer can sort again
  for (int i = 0; i < nCombos; i++) combos[i].used = false;

  tft.init();
  tft.setRotation(1);
  drawResult(bestIdx, verdict, colour);
}

void loop() {
  static uint32_t t = 0;
  if (millis() - t > 1000) {
    t = millis();
    Serial.printf("irq=%d\n", digitalRead(PIN_IRQ));
  }
  delay(10);
}
