/*
 * HIOTO Secured Switch — LVGL 8.4 configuration
 * Minimal overrides only; everything else falls back to LVGL defaults.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* RGB565; byte order for the panel is handled by TFT_eSPI pushColors(). */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Small heap: the UI is built once at startup. */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (64U * 1024U)

/* 20 ms refresh keeps a 170x320 panel responsive without hogging the CPU. */
#define LV_DISP_DEF_REFR_PERIOD 20
#define LV_INDEV_DEF_READ_PERIOD 20

/* System tick from Arduino millis() — no lv_tick_inc() needed. */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_DPI_DEF 130
#define LV_USE_LOG 0

/* Fonts used by the UI. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Dark theme by default. */
#define LV_THEME_DEFAULT_DARK 1

#endif /* LV_CONF_H */
