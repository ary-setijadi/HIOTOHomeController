#pragma once

/* ===========================================================================
 *  HIOTO General Monitor — board pin map
 *
 *  Two CYD variants are supported, selected by the PlatformIO environment:
 *    - Sunton ESP32-3248S035  3.5"  ST7796  320x480 native
 *    - Sunton ESP32-2432S028R 2.8"  ILI9341 240x320 native
 *
 *  SPI wiring is identical on both, only the panel driver, native size and the
 *  backlight pin differ (27 vs 21); those come from the build flags.
 * ======================================================================== */

#define PIN_LCD_MOSI 13
#define PIN_LCD_SCLK 14
#define PIN_LCD_MISO 12
#define PIN_LCD_CS   15
#define PIN_LCD_DC   2
#define PIN_LCD_RST  -1

#ifndef PIN_LCD_BL
#define PIN_LCD_BL 27 /* 3.5" board; the 2.8" env overrides it with 21 */
#endif

/* XPT2046 resistive touch — shared SPI bus */
#define PIN_TOUCH_CS  33
#define PIN_TOUCH_IRQ 36

/* Ambient light sensor, RGB LED, BOOT */
#define PIN_LDR 34
#define PIN_LED_R 4
#define PIN_LED_G 16
#define PIN_LED_B 17
#define PIN_BOOT 0

/* Driven in landscape (setRotation(1)), so the longer side is the width. */
#if (TFT_WIDTH) > (TFT_HEIGHT)
#define LCD_WIDTH (TFT_WIDTH)
#define LCD_HEIGHT (TFT_HEIGHT)
#else
#define LCD_WIDTH (TFT_HEIGHT)
#define LCD_HEIGHT (TFT_WIDTH)
#endif
