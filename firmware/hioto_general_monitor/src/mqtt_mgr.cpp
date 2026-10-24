#include "mqtt_mgr.h"
#include "config.h"
#include "registry.h"
#include "ui.h"
#include "ota.h"

#include <WiFi.h>
#include <PubSubClient.h>

static WiFiClient s_tcp;
static PubSubClient s_client(s_tcp);
static const Settings *s_cfg = nullptr;
static uint32_t s_last_try = 0;
static uint32_t s_last_telemetry = 0;
static uint32_t s_last_heartbeat = 0;

static char s_topic_status[48];
static char s_topic_telemetry[48];
static char s_topic_cmd[48];

static const char *kSubs[] = {"Aktuator", "Sensor"};
static const size_t kSubCount = sizeof(kSubs) / sizeof(kSubs[0]);

static void build_topics(void) {
  snprintf(s_topic_status, sizeof s_topic_status, TOPIC_STATUS, s_cfg->device_id);
  snprintf(s_topic_telemetry, sizeof s_topic_telemetry, TOPIC_TELEMETRY, s_cfg->device_id);
  snprintf(s_topic_cmd, sizeof s_topic_cmd, "iot/%s/cmd", s_cfg->device_id);
}

static void on_message(char *topic, byte *payload, unsigned int len);

void mqtt_begin(const Settings *s) {
  s_cfg = s;
  build_topics();
  s_client.setServer(s_cfg->mqtt_host, s_cfg->mqtt_port);
  s_client.setBufferSize(512);
  s_client.setKeepAlive(20);
  /* PubSubClient's connect() blocks for its socket timeout. At the 15 s default a
   * broker that is merely down freezes the panel for most of every retry cycle:
   * the UI stops updating and the serial console stops answering. Cap it. */
  s_client.setSocketTimeout(2);
  /* without this every incoming message is dropped: lamp states from the
   * broker and the OTA command channel both depend on it */
  s_client.setCallback(on_message);
  Serial.printf("[mqtt] target %s:%u dev=%s\n", s_cfg->mqtt_host, s_cfg->mqtt_port,
                s_cfg->device_id);
}

static void publish_telemetry(void) {
  char buf[192];
  snprintf(buf, sizeof buf,
           "{\"device\":\"%s\",\"role\":\"switch-panel\",\"fw\":\"%s\",\"floor\":\"%s\","
           "\"room\":\"%s\",\"ip\":\"%s\"}",
           s_cfg->device_id, FW_VERSION, g_settings.floor_label, g_settings.room_label,
           WiFi.localIP().toString().c_str());
  s_client.publish(s_topic_telemetry, buf, false);
}

static void publish_heartbeat(void) {
  char buf[48];
  snprintf(buf, sizeof buf, "%s#1", s_cfg->device_id);
  s_client.publish("Status", buf, false);
}

static bool connect_mqtt(void) {
  char lwt[96];
  snprintf(lwt, sizeof lwt, "{\"device\":\"%s\",\"status\":\"offline\"}", s_cfg->device_id);
  Serial.printf("[mqtt] connecting %s:%u ...\n", s_cfg->mqtt_host, s_cfg->mqtt_port);
  bool ok = s_client.connect(s_cfg->device_id, s_cfg->mqtt_user, s_cfg->mqtt_pass,
                             s_topic_status, 1, true, lwt);
  if (!ok) {
    Serial.printf("[mqtt] connect failed rc=%d\n", s_client.state());
    return false;
  }
  char online[96];
  snprintf(online, sizeof online, "{\"device\":\"%s\",\"status\":\"online\"}", s_cfg->device_id);
  s_client.publish(s_topic_status, online, true);
  for (size_t i = 0; i < kSubCount; i++)
    Serial.printf("[mqtt] sub %s -> %d\n", kSubs[i], s_client.subscribe(kSubs[i], 0));
  Serial.printf("[mqtt] sub %s -> %d\n", s_topic_cmd, s_client.subscribe(s_topic_cmd, 0));
  Serial.printf("[mqtt] online; watching Aktuator/Sensor + %s\n", s_topic_cmd);
  publish_telemetry();
  publish_heartbeat();
  return true;
}

/* <guid>#<0|1>  (Aktuator) or switch states (Sensor) */
static void on_message(char *topic, byte *payload, unsigned int len) {
  char buf[96];
  if (len >= sizeof buf) len = sizeof buf - 1;
  memcpy(buf, payload, len);
  buf[len] = '\0';

  /* command channel: {"ota":1} triggers a firmware check */
  if (topic && !strcmp(topic, s_topic_cmd)) {
    Serial.printf("[mqtt] cmd %s\n", buf);
    if (strstr(buf, "\"ota\"") || !strncmp(buf, "ota", 3)) ota_request_check();
    return;
  }

  char *hash = strchr(buf, '#');
  if (!hash) return;
  *hash = '\0';
  const char *guid = buf;
  const char *val = hash + 1;
  int ri = registry_find_guid(guid);
  if (ri < 0) return;
  bool on = (strcmp(val, "0") == 0); /* active-low: 0 = ON */
  registry_set_state(ri, on);
  ui_refresh_states();
}

void mqtt_poll(void) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!s_client.connected()) {
    /* Back off while the broker is unreachable: retrying every 5 s kept the
     * loop busy with failed connects instead of drawing the screen. */
    static uint32_t s_fails = 0;
    const uint32_t gap = s_fails < 3 ? 5000 : 30000;
    if (millis() - s_last_try > gap) {
      s_last_try = millis();
      if (connect_mqtt()) {
        s_fails = 0;
      } else if (s_fails < 100) {
        s_fails++;
      }
    }
    return;
  }
  s_client.loop();
  if (millis() - s_last_telemetry > TELEMETRY_PERIOD_MS) {
    s_last_telemetry = millis();
    publish_telemetry();
  }
  if (millis() - s_last_heartbeat > HIOTO_HEARTBEAT_MS) {
    s_last_heartbeat = millis();
    publish_heartbeat();
  }
}

bool mqtt_connected(void) { return s_client.connected(); }

const char *mqtt_state_text(void) { return s_client.connected() ? "MQTT OK" : "MQTT --"; }

bool mqtt_publish_actuator(const char *guid, int value) {
  if (!s_client.connected() || !guid || !guid[0]) return false;
  char buf[48];
  snprintf(buf, sizeof buf, "%s#%d", guid, value);
  bool ok = s_client.publish("Aktuator", buf, false);
  Serial.printf("[mqtt] actuator %s -> %s\n", buf, ok ? "ok" : "fail");
  return ok;
}
