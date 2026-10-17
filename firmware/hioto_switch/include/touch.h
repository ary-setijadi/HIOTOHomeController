#pragma once
#include <stdbool.h>

/* Scan candidate I2C buses for a GT911 touch controller and, when found,
 * register it as an LVGL input device. Returns true when touch is available. */
bool touch_init(void);

/* Human-readable result of the probe (bus/address/resolution). */
const char *touch_status_text(void);
