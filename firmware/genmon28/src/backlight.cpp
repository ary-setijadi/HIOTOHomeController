#include <Arduino.h>

#include "backlight.h"
#include "board_pins.h"

#define BL_LEDC_CHANNEL 7
#define BL_LEDC_FREQ 25000
#define BL_LEDC_BITS 10
#define BL_LEDC_MAX ((1 << BL_LEDC_BITS) - 1)

static bool s_ready = false;
static uint8_t s_pct = 90;

static uint8_t clamp_pct(int pct) {
  if (pct < BL_MIN_PCT) pct = BL_MIN_PCT;
  if (pct > BL_MAX_PCT) pct = BL_MAX_PCT;
  return (uint8_t)pct;
}

void backlight_begin(uint8_t pct) {
  if (!s_ready) {
    /* TFT_eSPI drove the pin high as a plain GPIO during init; take it over. */
    ledcSetup(BL_LEDC_CHANNEL, BL_LEDC_FREQ, BL_LEDC_BITS);
    ledcAttachPin(PIN_LCD_BL, BL_LEDC_CHANNEL);
    s_ready = true;
  }
  backlight_set(pct);
}

void backlight_set(uint8_t pct) {
  s_pct = clamp_pct(pct);
  if (!s_ready) {
    ledcSetup(BL_LEDC_CHANNEL, BL_LEDC_FREQ, BL_LEDC_BITS);
    ledcAttachPin(PIN_LCD_BL, BL_LEDC_CHANNEL);
    s_ready = true;
  }
  /* slight gamma so the low end feels linear to the eye */
  uint32_t linear = (uint32_t)s_pct * BL_LEDC_MAX / 100U;
  uint32_t duty = (linear * linear) / BL_LEDC_MAX;
  if (duty == 0 && s_pct > 0) duty = 1;
  ledcWrite(BL_LEDC_CHANNEL, duty);
}

uint8_t backlight_get(void) { return s_pct; }
