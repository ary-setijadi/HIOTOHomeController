#pragma once
#include <Arduino.h>

/* Runtime settings, persisted in NVS (namespace "hioto"). */
struct Settings {
  char wifi_ssid[33];
  char wifi_pass[65];
  char mqtt_host[64];
  uint16_t mqtt_port;
  char mqtt_user[33];
  char mqtt_pass[65];
  char device_id[33];
  int8_t relay_gpio; /* -1 = simulated */
  uint8_t brightness;
};

/* Single shared instance, loaded in setup() from NVS (defaults when absent). */
extern Settings g_settings;

void settings_load(Settings *s);
void settings_save(const Settings *s);
void settings_factory_reset(void);
