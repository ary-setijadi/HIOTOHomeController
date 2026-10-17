#pragma once

/* ---------------------------------------------------------------------------
 *  HIOTO Secured Switch — board pin map (Sunton ESP32-1732S019C)
 * ---------------------------------------------------------------------------
 *  Display pins below are the commonly verified mapping for the ESP32-S3
 *  variant of this board. If the screen stays blank/white, see "Alternate pin
 *  maps" in README.md — a few sellers ship CS=15 / SCLK=14.
 *
 *  The touch controller (GT911) pins are NOT documented by the vendor; the
 *  firmware auto-probes candidate I2C buses at boot (see touch.cpp) and logs
 *  what it finds on the serial console.
 * ------------------------------------------------------------------------- */

/* ST7789 170x320 SPI display */
#define PIN_LCD_MOSI 13
#define PIN_LCD_SCLK 12
#define PIN_LCD_CS   10
#define PIN_LCD_DC   11
#define PIN_LCD_RST  1
#define PIN_LCD_BL   14

/* BOOT button on the module (GPIO0, active low) — used as a fallback UI
 * input when no GT911 is detected, and for factory-reset later (milestone 2). */
#define PIN_BOOT 0

#define LCD_WIDTH  170
#define LCD_HEIGHT 320
