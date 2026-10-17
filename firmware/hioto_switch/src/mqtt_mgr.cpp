#include "mqtt_mgr.h"
#include "config.h"
#include "wifi_mgr.h"
#include "relay.h"
#include "ui.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

extern void ui_backlight_set(int pct); /* main.cpp */

static WiFiClient s_tcp;
static PubSubClient s_client(s_tcp);
static const Settings *s_cfg = nullptr;
static uint32_t s_last_connect_try = 0;
static uint32_t s_last_telemetry = 0;

static char s_topic_telemetry[48];
static char s_topic_commands[48];
static char s_topic_status[48];

static void build_topics(void) {
  snprintf(s_topic_telemetry, sizeof s_topic_telemetry, TOPIC_TELEMETRY, s_cfg->device_id);
  snprintf(s_topic_commands, sizeof s_topic_commands, TOPIC_COMMANDS, s_cfg->device_id);
  snprintf(s_topic_status, sizeof s_topic_status, TOPIC_STATUS, s_cfg->device_id);
}

static void publish_state_now(void) {
  char buf[192];
  snprintf(buf, sizeof buf,
           "{\"device\":\"%s\",\"relay\":%s,\"brightness\":%u,\"uptime_s\":%lu,\"rssi\":%d}",
           s_cfg->device_id, relay_get() ? "true" : "false", (unsigned)g_settings.brightness,
           (unsigned long)(millis() / 1000), WiFi.RSSI());
  s_client.publish(s_topic_telemetry, buf, false);
  Serial.printf("[mqtt] telemetry -> %s\n", buf);
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
  if (!strcmp(cmd, "set")) {
    bool on = doc["on"] | !relay_get();
    relay_set(on);
    ui_apply_external_state(on);
    publish_state_now();
  } else if (!strcmp(cmd, "toggle")) {
    relay_toggle();
    ui_apply_external_state(relay_get());
    publish_state_now();
  } else if (!strcmp(cmd, "brightness")) {
    int b = doc["value"] | g_settings.brightness;
    if (b < 20) b = 20;
    if (b > 100) b = 100;
    g_settings.brightness = (uint8_t)b;
    ui_backlight_set(b);
    ui_set_brightness((uint8_t)b);
    publish_state_now();
  } else if (!strcmp(cmd, "get") || !strcmp(cmd, "state")) {
    publish_state_now();
  } else if (!strcmp(cmd, "reboot")) {
    Serial.println("[mqtt] reboot requested");
    delay(200);
    ESP.restart();
  } else {
    Serial.printf("[mqtt] unknown command '%s'\n", cmd);
  }
}

void mqtt_mgr_begin(const Settings *s) {
  s_cfg = s;
  build_topics();
  s_client.setServer(s_cfg->mqtt_host, s_cfg->mqtt_port);
  s_client.setCallback(on_command);
  s_client.setBufferSize(512);
  Serial.printf("[mqtt] target %s:%u  dev=%s\n", s_cfg->mqtt_host, s_cfg->mqtt_port,
                s_cfg->device_id);
}

static bool connect_mqtt(void) {
  char lwt[80];
  snprintf(lwt, sizeof lwt, "{\"device\":\"%s\",\"status\":\"offline\"}", s_cfg->device_id);
  Serial.printf("[mqtt] connecting %s:%u ...\n", s_cfg->mqtt_host, s_cfg->mqtt_port);
  bool ok = s_client.connect(s_cfg->device_id, s_cfg->mqtt_user, s_cfg->mqtt_pass,
                             s_topic_status, 1, true, lwt);
  if (!ok) {
    Serial.printf("[mqtt] connect failed, rc=%d\n", s_client.state());
    return false;
  }
  char online[80];
  snprintf(online, sizeof online, "{\"device\":\"%s\",\"status\":\"online\"}", s_cfg->device_id);
  s_client.publish(s_topic_status, online, true);
  s_client.subscribe(s_topic_commands, 1);
  Serial.printf("[mqtt] online; subscribed %s\n", s_topic_commands);
  publish_state_now();
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
    publish_state_now();
  }
}

bool mqtt_mgr_connected(void) { return s_client.connected(); }

void mqtt_mgr_publish_state(void) {
  if (s_client.connected()) publish_state_now();
}

const char *mqtt_mgr_status_text(void) {
  return s_client.connected() ? "online" : "offline";
}
