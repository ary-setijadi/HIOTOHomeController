#pragma once
#include <Arduino.h>

/* Relay / switch output driver.
 * HARDWARE NOTE: the current basic board has no relay yet; gpio -1 keeps the
 * output simulated (state + serial log only). When the output is wired, pass
 * the real GPIO here or via the provisioning portal. */
void relay_init(int8_t gpio, int active_level);
void relay_set(bool on);
bool relay_get(void);
void relay_toggle(void);
