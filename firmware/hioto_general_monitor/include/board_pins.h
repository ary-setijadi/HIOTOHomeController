#pragma once

/* HIOTO General Monitor — board pin map (Sunton ESP32-3248S035) */

/* ST7796 320x480 native (driven as 480x320 landscape with rotation 1) */
#define PIN_LCD_MOSI 13
#define PIN_LCD_SCLK 14
#define PIN_LCD_MISO 12
#define PIN_LCD_CS   15
#define PIN_LCD_DC   2
#define PIN_LCD_RST  -1
#define PIN_LCD_BL   27

/* XPT2046 resistive touch — shared SPI bus */
#define PIN_TOUCH_CS  33
#define PIN_TOUCH_IRQ 36

/* Ambient light sensor, RGB LED, BOOT */
#define PIN_LDR 34
#define PIN_LED_R 4
#define PIN_LED_G 16
#define PIN_LED_B 17
#define PIN_BOOT 0

#define LCD_WIDTH  480
#define LCD_HEIGHT 320
