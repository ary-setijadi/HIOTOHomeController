#pragma once
#include <Arduino.h>

/* ===========================================================================
 *  Overall task watchdog (ESP32 TWDT)
 *
 *  The panel has no way to be power-cycled remotely, so the only recovery from
 *  a wedged loop - a stalled SPI/touch bus, a blocked HTTP call, an LVGL timer
 *  that never returns - is a reboot. The TWDT watches the Arduino loop task and
 *  panics when it has not been fed for WDT_TIMEOUT_S seconds; the panic handler
 *  reboots the board.
 *
 *  Feed it from loop() every iteration and from every long-running loop that
 *  does not return to loop() (touch calibration waits, the display self test,
 *  the OTA download write-loop). Never feed it from a timer or a second task:
 *  the value is only meaningful if it proves the loop task itself is alive.
 * ======================================================================== */

void wdt_begin(void);   /* init + subscribe the loop task (call once in setup) */
void wdt_feed(void);    /* reset the watchdog; safe to call when inactive */
bool wdt_active(void);  /* false if the core refused the WDT (then no recovery) */
uint32_t wdt_timeout_s(void);

/* Text for esp_reset_reason(), so a deliberate ESP.restart() can be told apart
 * from a panic, a watchdog reset or a brownout on the console. */
const char *wdt_reset_reason(void);
