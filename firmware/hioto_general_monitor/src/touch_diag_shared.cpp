/* ===========================================================================
 *  Touch diagnostics for the shared-bus topology (3.5" CYD / ESP32-3248S035)
 *
 *  On the 2.8" board the XPT2046 hangs off its own bus, so touch.cpp can
 *  bit-bang it and offer several sampling variants plus a verbatim port of the
 *  probe firmware. The 3.5" panel goes through TFT_eSPI instead and has no
 *  equivalent, but main.cpp's serial console calls both entry points on every
 *  build - so they are defined here for the shared-bus build only.
 * ======================================================================== */
#include <Arduino.h>

#include "config.h"
#include "touch.h"

#if !TOUCH_SEPARATE_BUS

/* The deep hardware sweep only makes sense for the bit-banged 2.8" bus. */
void touch_diag_full(void) {
  Serial.println("[tpd] diagnostik lengkap hanya tersedia pada board 2.8\"");
}

/* No bit-bang variants exist here: report the normal raw pressure so the
 * console command stays harmless instead of silently doing nothing. */
uint16_t touch_debug_variant(uint8_t cmd, int mode) {
  (void)cmd;
  (void)mode;
  return touch_raw_pressure();
}

/* Same contract as the 2.8" implementation: fill x/y/z and report whether a
 * usable sample was read. */
bool touch_probe_read(uint16_t *out_x, uint16_t *out_y, uint16_t *out_z) {
  uint16_t x = 0, y = 0;
  const uint16_t z = touch_raw_pressure();
  const bool ok = touch_raw(&x, &y);
  if (out_x) *out_x = x;
  if (out_y) *out_y = y;
  if (out_z) *out_z = z;
  return ok;
}

#endif
