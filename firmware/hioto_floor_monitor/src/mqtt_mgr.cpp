#include "mqtt_mgr.h"
#include "config.h"
#include "wifi_mgr.h"
#include "monitor.h"
#include "ui.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

extern void ui_backlight_set(int pct);      /* main.cpp */
extern void touch_calibrate_now(void);      /* main.cpp */

static WiFiClient s_tcp;
static PubSubClient s_client(s_tcp);
static const Settings *s_cfg = nullptr;
static uint32_t s_last_connect_try = 0;
static uint32_t s_last_telemetry = 0;

static char s_topic_telemetry[48];
static char s_topic_commands[48];
static char s_topic_status[48];
static char s_topic_suhu[48];
static uint32_t s_last_hb = 0;

/* Topics we listen to in order to build the floor view.
 * Aktuator carries the actuator state broadcast (guid#0|1, 0 = ON, 1 = OFF). */
static const char *kFloorSubs[] = {
    "iot/#", "Sensor", "Status", "Aktuator", "sensor_suhu/+", "sensor_gas_detector/+",
    "sensor_water_tank/+", "sensor_weather/+", "Log/+",
};
static const size_t kFloorSubCount = sizeof(kFloorSubs) / sizeof(kFloorSubs[0]);

static void build_topics(void) {
  snprintf(s_topic_telemetry, sizeof s_topic_telemetry, TOPIC_TELEMETRY, s_cfg->device_id);
  snprintf(s_topic_commands, sizeof s_topic_commands, TOPIC_COMMANDS, s_cfg->device_id);
  snprintf(s_topic_status, sizeof s_topic_status, TOPIC_STATUS, s_cfg->device_id);
  snprintf(s_topic_suhu, sizeof s_topic_suhu, TOPIC_SUHU, s_cfg->device_id);
}

/* HIOTO-compatible presence + temperature:
 *  - "Status"            "<guid>#1"  -> controller marks the device online
 *  - "sensor_suhu/<guid>" JSON        -> controller records the temperature
 */
static void publish_hioto_heartbeat(void) {
  char buf[192];
  snprintf(buf, sizeof buf, "%s#1", s_cfg->device_id);
  s_client.publish("Status", buf, false);

  MonEnv e;
  monitor_env(&e);
  snprintf(buf, sizeof buf,
           "{\"guid\":\"%s\",\"mac\":\"%s\",\"deviceName\":\"%s\","
           "\"value\":{\"temperature\":%.1f},\"unit\":{\"temperature\":\"C\"}}",
           s_cfg->device_id, WiFi.macAddress().c_str(), s_cfg->floor_label, e.die_temp_c);
  s_client.publish(s_topic_suhu, buf, false);
}

static void publish_telemetry_now(void) {
  char buf[320];
  monitor_build_telemetry(buf, sizeof buf);
  s_client.publish(s_topic_telemetry, buf, false);
  Serial.printf("[mqtt] telemetry -> %s\n", buf);
}

static void set_floor(const char *value) {
  if (!value || !value[0]) return;
  strncpy(g_settings.floor_label, value, sizeof g_settings.floor_label - 1);
  g_settings.floor_label[sizeof g_settings.floor_label - 1] = '\0';
  settings_save(&g_settings);
  Serial.printf("[cmd] floor -> %s\n", g_settings.floor_label);
  publish_telemetry_now();
}

static void on_command(char *topic, byte *payload, unsigned int len) {
  (void)topic;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, len);
  if (err) {
    Serial.printf("[mqtt] bad command JSON: %s\n", err.c_str());
    return;
  }
  const char *cmd = doc["cmd"] | "";
  Serial.printf("[mqtt] command: %s\n", cmd);
  if (!strcmp(cmd, "get") || !strcmp(cmd, "state")) {
    publish_telemetry_now();
  } else if (!strcmp(cmd, "reboot")) {
    Serial.println("[mqtt] reboot requested");
    delay(200);
    ESP.restart();
  } else if (!strcmp(cmd, "brightness")) {
    int b = doc["value"] | g_settings.brightness;
    if (b < 20) b = 20;
    if (b > 100) b = 100;
    g_settings.brightness = (uint8_t)b;
    ui_backlight_set(b);
    settings_save(&g_settings);
    publish_telemetry_now();
  } else if (!strcmp(cmd, "floor") || !strcmp(cmd, "set_floor")) {
    set_floor(doc["value"] | "");
  } else if (!strcmp(cmd, "calibrate") || !strcmp(cmd, "cal")) {
    Serial.println("[mqtt] touch calibration requested");
    touch_calibrate_now();
  } else {
    Serial.printf("[mqtt] unknown command '%s'\n", cmd);
  }
}

static void on_message(char *topic, byte *payload, unsigned int len) {
  if (!strcmp(topic, s_topic_commands)) {
    on_command(topic, payload, len);
    return;
  }
  monitor_feed(topic, payload, len);
}

void mqtt_mgr_begin(const Settings *s) {
  s_cfg = s;
  build_topics();
  s_client.setServer(s_cfg->mqtt_host, s_cfg->mqtt_port);
  s_client.setCallback(on_message);
  s_client.setBufferSize(1024);
  s_client.setKeepAlive(20);
  Serial.printf("[mqtt] target %s:%u  dev=%s\n", s_cfg->mqtt_host, s_cfg->mqtt_port,
                s_cfg->device_id);
}

static bool connect_mqtt(void) {
  char lwt[128];
  snprintf(lwt, sizeof lwt,
           "{\"device\":\"%s\",\"status\":\"offline\",\"role\":\"floor-monitor\",\"floor\":\"%s\"}",
           s_cfg->device_id, g_settings.floor_label);
  Serial.printf("[mqtt] connecting %s:%u ...\n", s_cfg->mqtt_host, s_cfg->mqtt_port);
  bool ok = s_client.connect(s_cfg->device_id, s_cfg->mqtt_user, s_cfg->mqtt_pass,
                             s_topic_status, 1, true, lwt);
  if (!ok) {
    Serial.printf("[mqtt] connect failed, rc=%d\n", s_client.state());
    return false;
  }
  char online[128];
  snprintf(online, sizeof online,
           "{\"device\":\"%s\",\"status\":\"online\",\"role\":\"floor-monitor\",\"floor\":\"%s\"}",
           s_cfg->device_id, g_settings.floor_label);
  s_client.publish(s_topic_status, online, true);
  s_client.subscribe(s_topic_commands, 1);
  for (size_t i = 0; i < kFloorSubCount; i++) s_client.subscribe(kFloorSubs[i], 0);
  Serial.printf("[mqtt] online; watching floor (iot/... + legacy HIOTO topics)\n");
  publish_telemetry_now();
  publish_hioto_heartbeat();
  s_last_hb = millis();
  return true;
}

void mqtt_mgr_poll(void) {
  if (wifi_mgr_state() != WIFI_ST_OK) return;
  if (!s_client.connected()) {
    if (millis() - s_last_connect_try > 5000) {
      s_last_connect_try = millis();
      connect_mqtt();
    }
    return;
  }
  s_client.loop();
  if (millis() - s_last_telemetry > TELEMETRY_PERIOD_MS) {
    s_last_telemetry = millis();
    publish_telemetry_now();
  }
  if (millis() - s_last_hb > HIOTO_HEARTBEAT_MS) {
    s_last_hb = millis();
    publish_hioto_heartbeat();
  }
}

bool mqtt_mgr_connected(void) { return s_client.connected(); }

void mqtt_mgr_publish_state(void) {
  if (s_client.connected()) publish_telemetry_now();
}

bool mqtt_publish_actuator(const char *guid, int value) {
  if (!s_client.connected() || !guid || !guid[0]) return false;
  char buf[48];
  snprintf(buf, sizeof buf, "%s#%d", guid, value);
  bool ok = s_client.publish("Aktuator", buf, false);
  Serial.printf("[mqtt] actuator %s -> %s\n", buf, ok ? "ok" : "fail");
  return ok;
}

const char *mqtt_mgr_status_text(void) {
  return s_client.connected() ? "online" : "offline";
}
