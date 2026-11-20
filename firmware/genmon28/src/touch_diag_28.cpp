/* ===========================================================================
 *  Decisive touch diagnostics for the 2.8" CYD (ESP32-2432S028)
 *
 *  The panel reports irq=LOW permanently with x=0 y~4078 z=0 through every
 *  sampling variant tried so far, so calibration has no valid raw values to
 *  fit. This routine separates the remaining hypotheses instead of guessing:
 *
 *    1. IRQ line behaviour            (stuck low vs. open input)
 *    2. MISO continuity               (floating pin vs. driven by the chip)
 *    3. XPT2046 over hardware VSPI    (the reference implementation at 1 MHz)
 *    4. XPT2046 over slow bit-bang    (rules out edge-timing problems)
 *    5. I2C scan on IO22/IO27         (finds a CST816/GT911 = "...028C" variant)
 *
 *  Call from the serial console with "tpdiag".
 * ======================================================================== */
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

#include "config.h"

#if TOUCH_SEPARATE_BUS

#define TP_CLK 25
#define TP_MOSI 32
#define TP_MISO 39
#define TP_CS 33
#define TP_IRQ 36

/* ---- 3. hardware VSPI, exactly as the reference documents ---------------- */
static uint16_t vspi_read(uint8_t cmd) {
  SPIClass tp(VSPI);
  tp.begin(TP_CLK, TP_MISO, TP_MOSI, TP_CS);
  tp.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TP_CS, LOW);
  uint8_t hi = tp.transfer(cmd);
  uint8_t lo = tp.transfer(0x00);
  digitalWrite(TP_CS, HIGH);
  tp.endTransaction();
  tp.end();
  return (uint16_t)((((uint16_t)hi << 8) | lo) >> 3) & 0x0FFF;
}

/* ---- 4. slow bit-bang: 5 us per half period, ~100 kHz -------------------- */
static uint16_t slow_read(uint8_t cmd) {
  pinMode(TP_CS, OUTPUT);
  pinMode(TP_CLK, OUTPUT);
  pinMode(TP_MOSI, OUTPUT);
  pinMode(TP_MISO, INPUT);
  digitalWrite(TP_CLK, LOW);
  digitalWrite(TP_MOSI, LOW);
  digitalWrite(TP_CS, HIGH);
  delayMicroseconds(20);

  digitalWrite(TP_CS, LOW);
  delayMicroseconds(5);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(TP_MOSI, (cmd >> i) & 1);
    delayMicroseconds(5);
    digitalWrite(TP_CLK, HIGH);
    delayMicroseconds(5);
    digitalWrite(TP_CLK, LOW);
  }
  uint16_t raw = 0;
  for (int i = 0; i < 16; i++) {
    digitalWrite(TP_CLK, HIGH);
    delayMicroseconds(5);
    raw = (uint16_t)((raw << 1) | (digitalRead(TP_MISO) & 1));
    digitalWrite(TP_CLK, LOW);
    delayMicroseconds(5);
  }
  digitalWrite(TP_CS, HIGH);
  return (uint16_t)(raw >> 3) & 0x0FFF;
}

void touch_diag_full(void) {
  Serial.println("[tpd] --- diagnostik layar sentuh 2.8\" ---");

  /* 1. IRQ ---------------------------------------------------------------- */
  pinMode(TP_IRQ, INPUT);
  int irq_run = 0;
  for (int i = 0; i < 200; i++) {
    irq_run += digitalRead(TP_IRQ) & 1;
    delayMicroseconds(200);
  }
  Serial.printf("[tpd] irq  pin%d: %d/200 sampel HIGH (0=selalu LOW)\n", TP_IRQ, irq_run);

  /* 2. MISO --------------------------------------------------------------- */
  pinMode(TP_MISO, INPUT);
  int miso_hi = 0;
  for (int i = 0; i < 200; i++) {
    miso_hi += digitalRead(TP_MISO) & 1;
    delayMicroseconds(200);
  }
  Serial.printf("[tpd] miso pin%d: %d/200 sampel HIGH (campur = pin mengambang)\n",
                TP_MISO, miso_hi);

  /* 3. hardware VSPI ------------------------------------------------------ */
  uint16_t vx = vspi_read(0xD0), vy = vspi_read(0x90), vz = vspi_read(0xB0);
  Serial.printf("[tpd] vspi 1MHz   x=%u y=%u z=%u\n", vx, vy, vz);

  /* 4. slow bit-bang ------------------------------------------------------ */
  uint16_t sx = slow_read(0xD0), sy = slow_read(0x90), sz = slow_read(0xB0);
  Serial.printf("[tpd] bitbang 100k x=%u y=%u z=%u\n", sx, sy, sz);

  /* repeat once: a real controller answers with similar numbers ----------- */
  uint16_t vx2 = vspi_read(0xD0), sy2 = slow_read(0x90);
  Serial.printf("[tpd] ulang         vspi.x=%u bitbang.y=%u (stabil?)\n", vx2, sy2);

  /* 5. capacitive controller hunt ---------------------------------------- */
  const int pairs[2][2] = {{22, 27}, {27, 22}};
  for (int p = 0; p < 2; p++) {
    Wire.begin(pairs[p][0], pairs[p][1]);
    delay(5);
    Serial.printf("[tpd] i2c sda=%d scl=%d:", pairs[p][0], pairs[p][1]);
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        Serial.printf(" 0x%02X", a);
        found++;
      }
    }
    if (!found) Serial.print(" (tidak ada)");
    Serial.println();
    Wire.end();
  }

  Serial.println("[tpd] selesai");
}

#endif /* TOUCH_SEPARATE_BUS */
