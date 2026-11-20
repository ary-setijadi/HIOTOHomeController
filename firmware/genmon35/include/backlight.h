#pragma once
#include <stdint.h>

/* ===========================================================================
 *  LCD backlight (PWM on PIN_LCD_BL) — dimmer for the switch panel.
 *  Percentage is clamped to 5..100 (0 would leave the panel unreadable).
 *
 *  IDLE DIMMING: a panel mounted on a wall should not glow all night. After
 *  BL_DIM_AFTER_MS with no touch it drops to BL_DIM_PCT of the configured
 *  level; the first touch restores it instantly. backlight_get() keeps
 *  reporting the CONFIGURED level so the Settings slider never looks as if it
 *  moved by itself.
 * ======================================================================== */

#define BL_MIN_PCT 5
#define BL_MAX_PCT 100

#define BL_DIM_AFTER_MS 300000UL /* 5 minutes without a touch */
#define BL_DIM_PCT 12            /* % of the configured level while idle */

void backlight_begin(uint8_t pct);
void backlight_set(uint8_t pct);
uint8_t backlight_get(void);

/* Call on any touch (or a BOOT press): refreshes the idle timer and, when the
 * panel is currently dimmed, restores full brightness immediately. */
void backlight_note_activity(void);

/* Call from the main loop: applies the idle dim once the timeout expires. */
void backlight_poll(void);

/* True while the panel is dimmed by the idle timeout. */
bool backlight_is_dimmed(void);
