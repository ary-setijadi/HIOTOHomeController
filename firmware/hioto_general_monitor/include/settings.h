#pragma once
#include <Arduino.h>
#include "config.h"

/* One lamp button slot: assigned HIOTO device + icon style. */
struct LampSlot {
  char guid[40]; /* actuator guid ("" = unassigned) */
  char label[16]; /* short id, e.g. "LAMP.12" */
  uint8_t icon;   /* 0 = spot lamp, 1 = general lamp */
};

struct Settings {
  char wifi_ssid[33];
  char wifi_pass[65];
  char mqtt_host[64];
  uint16_t mqtt_port;
  char mqtt_user[33];
  char mqtt_pass[65];
  char device_id[33];
  char floor_label[24];
  char room_label[24];
  LampSlot slots[GEN_SLOTS];
  uint8_t brightness;
  uint16_t touch_cal[8];
  bool touch_cal_valid;
};

extern Settings g_settings;

void settings_load(Settings *s);
void settings_save(const Settings *s);
void settings_factory_reset(void);
