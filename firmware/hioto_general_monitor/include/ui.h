#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Build the main + settings UI. */
void ui_init(void);

/* Refresh lamp button visuals + Wi-Fi/MQTT status line (call ~4x/s). */
void ui_refresh_states(void);

/* Housekeeping: applies a pending theme rebuild. Call once per loop(). */
void ui_poll(void);

/* Request a full re-style (used after a theme change from the console). */
void ui_theme_changed(void);

/* Open the settings screen (also used on first boot without Wi-Fi). */
void ui_open_settings(void);

/* Called after a registry refresh: rebuild the settings device dropdowns. */
void ui_registry_changed(void);

/* Wi-Fi helpers, implemented in main.cpp. */
bool wifi_ok(void);
const char *wifi_state_text(void);
void wifi_apply(const char *ssid, const char *pass);
