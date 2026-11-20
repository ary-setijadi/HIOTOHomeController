#include <Arduino.h>

#include "backlight.h"
#include "board_pins.h"

#define BL_LEDC_CHANNEL 7
#define BL_LEDC_FREQ 25000
#define BL_LEDC_BITS 10
#define BL_LEDC_MAX ((1 << BL_LEDC_BITS) - 1)

static bool s_ready = false;
static uint8_t s_pct = 90;        /* configured level - what Settings shows */
static bool s_dimmed = false;     /* idle dim currently applied */
static uint32_t s_last_touch = 0; /* millis() of the last touch / BOOT press */

static uint8_t clamp_pct(int pct) {
  if (pct < BL_MIN_PCT) pct = BL_MIN_PCT;
  if (pct > BL_MAX_PCT) pct = BL_MAX_PCT;
  return (uint8_t)pct;
}

/* The only place that touches the PWM hardware. */
static void apply_pct(uint8_t pct) {
  if (!s_ready) {
    /* TFT_eSPI drove the pin high as a plain GPIO during init; take it over. */
    ledcSetup(BL_LEDC_CHANNEL, BL_LEDC_FREQ, BL_LEDC_BITS);
    ledcAttachPin(PIN_LCD_BL, BL_LEDC_CHANNEL);
    s_ready = true;
  }
  /* slight gamma so the low end feels linear to the eye */
  uint32_t linear = (uint32_t)pct * BL_LEDC_MAX / 100U;
  uint32_t duty = (linear * linear) / BL_LEDC_MAX;
  if (duty == 0 && pct > 0) duty = 1;
  ledcWrite(BL_LEDC_CHANNEL, duty);
}

void backlight_begin(uint8_t pct) {
  s_pct = clamp_pct(pct);
  s_dimmed = false;
  s_last_touch = millis();
  apply_pct(s_pct);
}

/* Explicit brightness change (Settings slider / `bright` command). Always
 * means "the user is here", so it also clears a pending dim. */
void backlight_set(uint8_t pct) {
  s_pct = clamp_pct(pct);
  s_dimmed = false;
  s_last_touch = millis();
  apply_pct(s_pct);
}

uint8_t backlight_get(void) { return s_pct; }
bool backlight_is_dimmed(void) { return s_dimmed; }

void backlight_note_activity(void) {
  s_last_touch = millis();
  if (s_dimmed) {
    s_dimmed = false;
    apply_pct(s_pct); /* wake on the very first touch, not on the next poll */
  }
}

void backlight_poll(void) {
  if (!s_ready || s_dimmed) return;
  if (millis() - s_last_touch < BL_DIM_AFTER_MS) return;

  /* Never dim to black: at the minimum configured brightness a strict
   * percentage would land on the BL_MIN_PCT floor anyway. */
  uint32_t dim = (uint32_t)s_pct * BL_DIM_PCT / 100U;
  if (dim < BL_MIN_PCT) dim = BL_MIN_PCT;

  s_dimmed = true;
  apply_pct((uint8_t)dim);
  Serial.printf("[bl] idle %lus -> dim %u%% (configured %u%%)\n",
                (unsigned)((millis() - s_last_touch) / 1000), (unsigned)dim,
                (unsigned)s_pct);
}
