#pragma once
#include <Arduino.h>
#include "config.h"

/* One lamp button slot: assigned HIOTO device + icon style. */
struct LampSlot {
  char guid[40];  /* actuator guid ("" = unassigned) */
  char label[20]; /* compact id, e.g. "LAMP.12" / "SMARTSTEKER-16" */
  char alias[20]; /* friendly name shown on the button ("" = derive from label) */
  uint8_t icon;   /* see ICON_NAMES in config.h */
};

struct Settings {
  char wifi_ssid[33];
  char wifi_pass[65];
  char mqtt_host[64];
  uint16_t mqtt_port;
  char mqtt_user[33];
  char mqtt_pass[65];
  char device_id[33];
  char guid[40]; /* persistent controller GUID (UUID v4), unique per board */
  char floor_label[24];
  char room_label[24];
  uint8_t floor_id; /* controller floor id (0 = unset) */
  uint8_t room_id;  /* controller room id (0 = whole floor) */
  LampSlot slots[GEN_SLOTS];
  uint8_t brightness; /* LCD backlight, 5..100 % */
  uint8_t theme;      /* colour theme index (theme.h) */
  uint8_t scope;      /* device picker filter: 0 = room, 1 = floor, 2 = all */
  uint8_t ota_auto;   /* 1 = check the OTA server automatically */
  uint16_t touch_cal[8];
  bool touch_cal_valid;
};

extern Settings g_settings;

void settings_load(Settings *s);
void settings_save(const Settings *s);
void settings_factory_reset(void);
