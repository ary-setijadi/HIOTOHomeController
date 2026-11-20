#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <stdbool.h>
#include <stdint.h>

/* ===========================================================================
 *  Touch input, two wiring topologies:
 *
 *  3.5" CYD (ESP32-3248S035) — XPT2046 shares the display SPI bus, so the
 *      TFT_eSPI touch extension handles it (TOUCH_CS=33).
 *
 *  2.8" CYD (ESP32-2432S028) — XPT2046 sits on its OWN bus:
 *      CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 (LOW when touched).
 *      The display owns HSPI, so touch runs on VSPI and is driven here.
 *
 *  Both paths expose the same API and store calibration in NVS through
 *  g_settings.touch_cal. For the separate bus the layout is
 *    cal[0]=xMin cal[1]=xMax cal[2]=yMin cal[3]=yMax cal[4]=flags
 *  with flags bit0 = swapXY, bit1 = invertX, bit2 = invertY.
 * ======================================================================== */

void touch_begin(TFT_eSPI *tft);
void touch_set_screen(int w, int h);

/* Screen coordinates, false when the panel is not being touched. */
bool touch_point(int *x, int *y);
bool touch_is_down(void);
bool touch_raw(uint16_t *rx, uint16_t *ry); /* false when no valid sample */
uint16_t touch_raw_pressure(void);         /* raw Z sample, 0 when untouched */
void touch_debug_sample(uint16_t *z, uint16_t *x, uint16_t *y); /* no filtering */
uint16_t touch_debug_variant(uint8_t cmd, int mode); /* edge-sampling variants */
bool touch_probe_read(uint16_t *x, uint16_t *y, uint16_t *z); /* probe-firmware port */

/* Full screen 4-point calibration; draws straight on the panel and saves. */
void touch_calibrate_ui(void);
