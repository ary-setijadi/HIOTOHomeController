#pragma once
#include <stdint.h>

/* ===========================================================================
 *  LCD backlight (PWM on PIN_LCD_BL) — dimmer for the switch panel.
 *  Percentage is clamped to 5..100 (0 would leave the panel unreadable).
 * ======================================================================== */

#define BL_MIN_PCT 5
#define BL_MAX_PCT 100

void backlight_begin(uint8_t pct);
void backlight_set(uint8_t pct);
uint8_t backlight_get(void);
