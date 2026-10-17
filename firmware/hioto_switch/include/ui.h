#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Build the main UI screen. */
void ui_init(void);

/* BOOT-button path: toggle relay, update UI, publish state to MQTT. */
void ui_toggle_switch(void);

/* Apply a state that came from MQTT (updates UI + relay without re-publishing). */
void ui_apply_external_state(bool on);

/* Reflect a brightness change that came from MQTT. */
void ui_set_brightness(uint8_t pct);

/* Top status line + footer (footer blank = show device id). */
void ui_set_net_status(const char *status_line, const char *footer_line);
