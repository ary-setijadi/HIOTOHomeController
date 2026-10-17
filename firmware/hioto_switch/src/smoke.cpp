/* Minimal serial smoke test — used during board bring-up to prove that the
 * framework boots and UART0 (CH340 @ COM3) works, with no display/PSRAM code. */
#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== SMOKE OK ===");
  Serial.printf("chip=%s rev=%u cores=%u flash=%uKB psram=%uKB\n",
                ESP.getChipModel(), (unsigned)ESP.getChipRevision(),
                (unsigned)ESP.getChipCores(),
                (unsigned)(ESP.getFlashChipSize() / 1024),
                (unsigned)(ESP.getPsramSize() / 1024));
}

void loop() {
  static uint32_t n = 0;
  if ((n++ % 10) == 0) Serial.printf("smoke tick %lu\n", (unsigned long)n);
  delay(500);
}
