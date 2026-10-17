#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Build the LVGL UI (dashboard + detail + info views). */
void ui_init(void);

/* Cycle dashboard -> detail -> info -> dashboard (BOOT short press). */
void ui_next_page(void);

/* Show a specific page: 0 dashboard, 1 detail, 2 info. */
void ui_show_page(int page);
int ui_page(void);

/* Select a node for the detail page (by id). */
void ui_detail_select(const char *node_id);

/* Hook run by the "Kalibrasi Layar" button (installed by main.cpp). */
typedef void (*ui_cal_hook_t)(void);
void ui_set_cal_hook(ui_cal_hook_t fn);

/* Refresh all live labels from the floor model (call ~4x/s from loop). */
void ui_refresh(void);
