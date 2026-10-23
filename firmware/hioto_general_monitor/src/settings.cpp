#include "settings.h"
#include "config.h"
#include "theme.h"
#include <Preferences.h>
#include <WiFi.h>

Settings g_settings;

static void copy_str(char *dst, size_t dst_sz, const char *src) {
  if (!src) src = "";
  strncpy(dst, src, dst_sz - 1);
  dst[dst_sz - 1] = '\0';
}

static void derive_device_id(Settings *s) {
  if (s->device_id[0] != '\0') return;
  uint8_t raw[6] = {0};
  WiFi.macAddress(raw);
  snprintf(s->device_id, sizeof s->device_id, "hioto-genmon-%02x%02x%02x", raw[3],
           raw[4], raw[5]);
}

void settings_load(Settings *s) {
  memset(s, 0, sizeof *s);
  copy_str(s->wifi_ssid, sizeof s->wifi_ssid, "");
  copy_str(s->wifi_pass, sizeof s->wifi_pass, "");
  copy_str(s->mqtt_host, sizeof s->mqtt_host, DEFAULT_MQTT_HOST);
  s->mqtt_port = DEFAULT_MQTT_PORT;
  copy_str(s->mqtt_user, sizeof s->mqtt_user, DEFAULT_MQTT_USER);
  copy_str(s->mqtt_pass, sizeof s->mqtt_pass, DEFAULT_MQTT_PASS);
  copy_str(s->device_id, sizeof s->device_id, DEFAULT_DEVICE_ID);
  copy_str(s->floor_label, sizeof s->floor_label, DEFAULT_FLOOR);
  copy_str(s->room_label, sizeof s->room_label, DEFAULT_ROOM);
  s->brightness = DEFAULT_BRIGHTNESS;
  s->theme = DEFAULT_THEME;
  s->scope = DEFAULT_SCOPE;
  {
    const char *labels[GEN_SLOTS] = {DEFAULT_SLOT_LABELS};
    for (int i = 0; i < GEN_SLOTS; i++) {
      copy_str(s->slots[i].label, sizeof s->slots[i].label, labels[i]);
      s->slots[i].icon = (i < 4) ? 0 : 1; /* first row spot, second row general */
    }
  }

  Preferences prefs;
  if (!prefs.begin("hioto", true)) {
    derive_device_id(s);
    return;
  }
  prefs.getString("ssid", s->wifi_ssid, sizeof s->wifi_ssid);
  prefs.getString("pass", s->wifi_pass, sizeof s->wifi_pass);
  prefs.getString("mhost", s->mqtt_host, sizeof s->mqtt_host);
  s->mqtt_port = (uint16_t)prefs.getUShort("mport", s->mqtt_port);
  prefs.getString("muser", s->mqtt_user, sizeof s->mqtt_user);
  prefs.getString("mpass", s->mqtt_pass, sizeof s->mqtt_pass);
  prefs.getString("devid", s->device_id, sizeof s->device_id);
  prefs.getString("floor", s->floor_label, sizeof s->floor_label);
  prefs.getString("room", s->room_label, sizeof s->room_label);
  s->brightness = (uint8_t)prefs.getUChar("bright", s->brightness);
  s->theme = (uint8_t)prefs.getUChar("theme", s->theme);
  s->scope = (uint8_t)prefs.getUChar("scope", s->scope);
  s->touch_cal_valid = prefs.getBytes("tcal", s->touch_cal, sizeof s->touch_cal) ==
                       sizeof s->touch_cal;
  for (int i = 0; i < GEN_SLOTS; i++) {
    char key[8];
    snprintf(key, sizeof key, "s%dg", i);
    prefs.getString(key, s->slots[i].guid, sizeof s->slots[i].guid);
    snprintf(key, sizeof key, "s%dl", i);
    prefs.getString(key, s->slots[i].label, sizeof s->slots[i].label);
    snprintf(key, sizeof key, "s%di", i);
    s->slots[i].icon = prefs.getUChar(key, s->slots[i].icon);
  }
  prefs.end();

  if (s->brightness < 20) s->brightness = 20;
  if (s->brightness > 100) s->brightness = 100;
  if (s->theme >= THEME_COUNT) s->theme = DEFAULT_THEME;
  if (s->scope > 2) s->scope = DEFAULT_SCOPE;
  derive_device_id(s);
}

void settings_save(const Settings *s) {
  Preferences prefs;
  if (!prefs.begin("hioto", false)) return;
  prefs.putString("ssid", s->wifi_ssid);
  prefs.putString("pass", s->wifi_pass);
  prefs.putString("mhost", s->mqtt_host);
  prefs.putUShort("mport", s->mqtt_port);
  prefs.putString("muser", s->mqtt_user);
  prefs.putString("mpass", s->mqtt_pass);
  prefs.putString("devid", s->device_id);
  prefs.putString("floor", s->floor_label);
  prefs.putString("room", s->room_label);
  prefs.putUChar("bright", s->brightness);
  prefs.putUChar("theme", s->theme);
  prefs.putUChar("scope", s->scope);
  if (s->touch_cal_valid) prefs.putBytes("tcal", s->touch_cal, sizeof s->touch_cal);
  for (int i = 0; i < GEN_SLOTS; i++) {
    char key[8];
    snprintf(key, sizeof key, "s%dg", i);
    prefs.putString(key, s->slots[i].guid);
    snprintf(key, sizeof key, "s%dl", i);
    prefs.putString(key, s->slots[i].label);
    snprintf(key, sizeof key, "s%di", i);
    prefs.putUChar(key, s->slots[i].icon);
  }
  prefs.end();
}

void settings_factory_reset(void) {
  Preferences prefs;
  if (prefs.begin("hioto", false)) {
    prefs.clear();
    prefs.end();
  }
}
