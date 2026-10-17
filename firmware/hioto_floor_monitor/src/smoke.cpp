/**
 * Minimal bring-up smoke test for the 3.5" CYD (no display, no LVGL).
 * Flashes quickly and prints chip info + exercises the RGB LED.
 *
 *   python -m platformio run -e smoke -t upload   (manual download mode!)
 */
#include <Arduino.h>
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== HIOTO smoke test / 3248S035 ===");
  Serial.printf("chip   : %s rev %u\n", ESP.getChipModel(), (unsigned)ESP.getChipRevision());
  Serial.printf("cores  : %u\n", (unsigned)ESP.getChipCores());
  Serial.printf("flash  : %u KB\n", (unsigned)(ESP.getFlashChipSize() / 1024));
  Serial.printf("psram  : %u KB\n", (unsigned)(ESP.getPsramSize() / 1024));
  Serial.printf("mac    : %s\n", WiFi.macAddress().c_str());

  pinMode(4, OUTPUT);
  pinMode(16, OUTPUT);
  pinMode(17, OUTPUT);
  for (int i = 0; i < 3; i++) {
    digitalWrite(4, LOW); digitalWrite(16, HIGH); digitalWrite(17, HIGH);
    delay(300);
    digitalWrite(4, HIGH); digitalWrite(16, LOW); digitalWrite(17, HIGH);
    delay(300);
    digitalWrite(4, HIGH); digitalWrite(16, HIGH); digitalWrite(17, LOW);
    delay(300);
  }
  digitalWrite(4, HIGH);
  digitalWrite(16, HIGH);
  digitalWrite(17, HIGH);
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 1000) {
    last = millis();
    Serial.printf("alive  uptime=%lus  heap=%u  rssi=%d\n", (unsigned long)(millis() / 1000),
                  (unsigned)ESP.getFreeHeap(), WiFi.RSSI());
  }
}
