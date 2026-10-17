#pragma once

/* ---------------------------------------------------------------------------
 *  HIOTO Floor Monitor — board pin map (Sunton ESP32-3248S035, 3.5" CYD)
 * ---------------------------------------------------------------------------
 *  Verified against the esp3d.io hardware page for the 3248S035R/C family:
 *    Display  : ST7796 480x320, SPI (HSPI default pins)
 *    Touch    : XPT2046 resistive, shares the display SPI bus (CS = 33);
 *               the capacitive variant (3248S035C) uses GT911 on I2C.
 *    LDR      : GPIO34 (photoresistor, ADC1_CH6)
 *    RGB LED  : 4 / 16 / 17 (active LOW)
 *    Speaker  : GPIO26 (PWM amp, unused here)
 *    SD card  : VSPI 5/18/19/23 (unused here)
 *    Buttons  : BOOT on GPIO0; EN resets the board (tied to TFT_RST too)
 * ------------------------------------------------------------------------- */

/* ST7796 480x320 SPI display */
#define PIN_LCD_MOSI 13
#define PIN_LCD_SCLK 14
#define PIN_LCD_MISO 12
#define PIN_LCD_CS   15
#define PIN_LCD_DC   2
#define PIN_LCD_RST  -1 /* tied to EN */
#define PIN_LCD_BL   27

/* XPT2046 resistive touch — shared SPI bus, separate chip-select. */
#define PIN_TOUCH_CS  33
#define PIN_TOUCH_IRQ 36

/* Ambient light sensor (photoresistor). */
#define PIN_LDR 34

/* RGB status LED (active LOW). */
#define PIN_LED_R 4
#define PIN_LED_G 16
#define PIN_LED_B 17

/* BOOT button on the module (GPIO0, active low). */
#define PIN_BOOT 0

#define LCD_WIDTH  480
#define LCD_HEIGHT 320
