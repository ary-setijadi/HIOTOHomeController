#include "relay.h"

static int8_t s_gpio = -1;
static int s_active_level = HIGH;
static bool s_state = false;

void relay_init(int8_t gpio, int active_level) {
  s_gpio = gpio;
  s_active_level = active_level;
  s_state = false;
  if (s_gpio >= 0) {
    pinMode(s_gpio, OUTPUT);
    digitalWrite(s_gpio, s_active_level == HIGH ? LOW : HIGH); /* off */
  }
  Serial.printf("[relay] %s gpio=%d active=%d\n", s_gpio >= 0 ? "ready" : "SIMULATED",
                (int)s_gpio, s_active_level);
}

void relay_set(bool on) {
  s_state = on;
  if (s_gpio >= 0) {
    digitalWrite(s_gpio, on ? s_active_level : !s_active_level);
    Serial.printf("[relay] gpio%d -> %s\n", (int)s_gpio, on ? "ON" : "OFF");
  } else {
    Serial.printf("[relay] output -> %s (simulated)\n", on ? "ON" : "OFF");
  }
}

bool relay_get(void) { return s_state; }

void relay_toggle(void) { relay_set(!s_state); }
