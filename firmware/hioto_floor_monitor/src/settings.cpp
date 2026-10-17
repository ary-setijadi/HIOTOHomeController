#include "settings.h"
#include "config.h"
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
  snprintf(s->device_id, sizeof s->device_id, "hioto-floor-%02x%02x%02x",
           raw[3], raw[4], raw[5]);
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
  s->floor_id = DEFAULT_FLOOR_ID;
  copy_str(s->ctrl_host, sizeof s->ctrl_host, DEFAULT_CTRL_HOST);
  s->ctrl_port = DEFAULT_CTRL_PORT;
  s->brightness = 90;

  Preferences prefs;
  if (!prefs.begin("hioto", true)) { /* read-only; namespace may not exist */
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
  s->floor_id = (uint8_t)prefs.getUChar("fid", s->floor_id);
  prefs.getString("chost", s->ctrl_host, sizeof s->ctrl_host);
  s->ctrl_port = (uint16_t)prefs.getUShort("cport", s->ctrl_port);
  s->brightness = (uint8_t)prefs.getUChar("bright", s->brightness);
  s->touch_cal_valid = prefs.getBytes("tcal", s->touch_cal, sizeof s->touch_cal) ==
                       sizeof s->touch_cal;
  prefs.end();

  if (s->brightness < 20) s->brightness = 20;
  if (s->brightness > 100) s->brightness = 100;
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
  prefs.putUChar("fid", s->floor_id);
  prefs.putString("chost", s->ctrl_host);
  prefs.putUShort("cport", s->ctrl_port);
  prefs.putUChar("bright", s->brightness);
  if (s->touch_cal_valid) prefs.putBytes("tcal", s->touch_cal, sizeof s->touch_cal);
  prefs.end();
}

void settings_factory_reset(void) {
  Preferences prefs;
  if (prefs.begin("hioto", false)) {
    prefs.clear();
    prefs.end();
  }
}
