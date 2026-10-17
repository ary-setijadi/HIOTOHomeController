#include "monitor.h"
#include "config.h"
#include "settings.h"
#include "board_pins.h"
#include "registry.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp32-hal-adc.h>

extern float temperatureRead(); /* esp32-hal: classic ESP32 SoC sensor */

static MonNode s_nodes[MON_MAX_NODES];
static int s_count = 0;

static uint32_t now_ms(void) { return millis(); }

/* --- helpers ------------------------------------------------------------- */

static void copy_str(char *dst, size_t dst_sz, const char *src) {
  if (!src) src = "";
  strncpy(dst, src, dst_sz - 1);
  dst[dst_sz - 1] = '\0';
}

/* Pick the "most interesting" value from a telemetry JSON object. */
static void summarize_json(JsonDocument &doc, char *out, size_t n) {
  out[0] = '\0';
  if (doc["temp_c"].is<float>() || doc["temp_c"].is<int>()) {
    snprintf(out, n, "%.1fC", doc["temp_c"].as<float>());
  } else if (doc["temperature"].is<float>() || doc["temperature"].is<int>()) {
    snprintf(out, n, "%.1fC", doc["temperature"].as<float>());
  } else if (doc["humidity"].is<float>() || doc["humidity"].is<int>()) {
    snprintf(out, n, "%.0f%% RH", doc["humidity"].as<float>());
  } else if (doc["relay"].is<bool>()) {
    snprintf(out, n, "%s", doc["relay"].as<bool>() ? "ON" : "OFF");
  } else if (doc["power"].is<float>() || doc["power"].is<int>()) {
    snprintf(out, n, "%.1fW", doc["power"].as<float>());
  } else if (doc["status"].is<int>()) {
    snprintf(out, n, "status=%d", doc["status"].as<int>());
  } else {
    /* Fallback: first string/number member. */
    JsonObject o = doc.as<JsonObject>();
    for (JsonPair kv : o) {
      const char *k = kv.key().c_str();
      if (kv.value().is<const char *>()) {
        snprintf(out, n, "%s=%s", k, kv.value().as<const char *>());
      } else if (kv.value().is<float>() || kv.value().is<int>()) {
        snprintf(out, n, "%s=%g", k, kv.value().as<double>());
      } else if (kv.value().is<bool>()) {
        snprintf(out, n, "%s=%s", k, kv.value().as<bool>() ? "true" : "false");
      }
      if (out[0]) break;
    }
  }
  if (out[0] == '\0') copy_str(out, n, "-");
}

/* Insert (sorted by id) or update a node. Returns the node.
 * Once the floor registry is loaded, only nodes on this floor are tracked. */
static MonNode *upsert(const char *id) {
  if (!id || !id[0]) return NULL;
  if (registry_ready() && !registry_contains(id)) return NULL;
  for (int i = 0; i < s_count; i++) {
    if (!strcmp(s_nodes[i].id, id)) return &s_nodes[i];
  }
  MonNode *slot = NULL;
  if (s_count < MON_MAX_NODES) {
    slot = &s_nodes[s_count];
  } else {
    /* Evict the least-recently-seen node. */
    slot = &s_nodes[0];
    for (int i = 1; i < s_count; i++) {
      if (s_nodes[i].last_ms < slot->last_ms) slot = &s_nodes[i];
    }
  }
  memset(slot, 0, sizeof *slot);
  copy_str(slot->id, sizeof slot->id, id);
  copy_str(slot->label, sizeof slot->label, id);
  strcpy(slot->kind, "sensor");
  strcpy(slot->value, "-");
  slot->last_ms = now_ms();
  if (s_count < MON_MAX_NODES) s_count++;
  return slot;
}

/* Keep the array sorted by id for a stable dashboard ordering. */
static void sort_nodes(void) {
  for (int i = 1; i < s_count; i++) {
    MonNode t = s_nodes[i];
    int j = i - 1;
    while (j >= 0 && strcmp(s_nodes[j].id, t.id) > 0) {
      s_nodes[j + 1] = s_nodes[j];
      j--;
    }
    s_nodes[j + 1] = t;
  }
}

/* Use the registry name as the human label and copy its room. */
static void label_from_registry(MonNode *n) {
  const char *nm = registry_name(n->id);
  if (nm && nm[0]) copy_str(n->label, sizeof n->label, nm);
  const char *rm = registry_room_of(n->id);
  copy_str(n->room, sizeof n->room, (rm && rm[0]) ? rm : "Lainnya");
  n->actuator = registry_is_actuator(n->id);
}

/* Drop nodes that are not part of this floor (after the registry loads). */
void monitor_prune(void) {
  if (!registry_ready()) return;
  int w = 0;
  for (int i = 0; i < s_count; i++) {
    if (registry_contains(s_nodes[i].id)) s_nodes[w++] = s_nodes[i];
  }
  s_count = w;
}

/* Refresh label + room of every tracked node from the registry. */
void monitor_apply_registry(void) {
  for (int i = 0; i < s_count; i++) label_from_registry(&s_nodes[i]);
}

/* --- topic ingestion ----------------------------------------------------- */

/* iot/{id}/telemetry | status */
static void feed_iot(const char *topic, const uint8_t *payload, size_t len) {
  /* topic already starts with "iot/" */
  const char *p = topic + 4;
  const char *slash = strchr(p, '/');
  if (!slash) return;
  char id[40];
  size_t idlen = (size_t)(slash - p);
  if (idlen == 0 || idlen >= sizeof id) return;
  memcpy(id, p, idlen);
  id[idlen] = '\0';

  /* Never list ourselves — we are the panel, not a floor node. */
  if (!strcmp(id, g_settings.device_id)) return;

  const char *suffix = slash + 1;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, len);
  MonNode *n = upsert(id);
  if (!n) return;
  n->last_ms = now_ms();
  label_from_registry(n);

  if (!strcmp(suffix, "status")) {
    strcpy(n->kind, "status");
    if (doc["device"].is<const char *>()) copy_str(n->label, sizeof n->label, doc["device"].as<const char *>());
    const char *st = doc["status"] | "online";
    n->online = strcmp(st, "offline") != 0;
    copy_str(n->value, sizeof n->value, "-");
  } else if (!strcmp(suffix, "telemetry")) {
    strcpy(n->kind, "telemetry");
    n->online = true;
    if (!err && doc["device"].is<const char *>())
      copy_str(n->label, sizeof n->label, doc["device"].as<const char *>());
    if (!err) {
      if (doc["relay"].is<bool>()) strcpy(n->kind, "relay");
      summarize_json(doc, n->value, sizeof n->value);
    }
  } else {
    /* commands / other: just count as alive, keep previous info */
    n->online = true;
  }
  sort_nodes();
}

/* Legacy HIOTO topics: Sensor / Status = "guid#value" */
static void feed_guid_value(const char *topic, const uint8_t *payload, size_t len) {
  char buf[96];
  if (len >= sizeof buf) len = sizeof buf - 1;
  memcpy(buf, payload, len);
  buf[len] = '\0';
  char *hash = strchr(buf, '#');
  if (!hash) return;
  *hash = '\0';
  const char *id = buf;
  const char *val = hash + 1;
  MonNode *n = upsert(id);
  if (!n) return;
  n->last_ms = now_ms();
  n->online = true;
  label_from_registry(n);
  if (!strcmp(topic, "Status")) {
    strcpy(n->kind, "status");
    copy_str(n->value, sizeof n->value, "-");
  } else {
    strcpy(n->kind, "relay");
    /* HIOTO lamps/relays are ACTIVE-LOW: 0 = ON, 1 = OFF. */
    if (!strcmp(val, "0")) copy_str(n->value, sizeof n->value, "ON");
    else if (!strcmp(val, "1")) copy_str(n->value, sizeof n->value, "OFF");
    else copy_str(n->value, sizeof n->value, val);
  }
  sort_nodes();
}

/* Legacy sensor_* JSON topics. */
static void feed_sensor_json(const char *topic, const uint8_t *payload, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  const char *id = doc["guid"].is<const char *>() ? doc["guid"].as<const char *>() : NULL;
  if (!id || !id[0]) return;
  MonNode *n = upsert(id);
  if (!n) return;
  n->last_ms = now_ms();
  n->online = true;
  label_from_registry(n);

  if (doc["deviceName"].is<const char *>())
    copy_str(n->label, sizeof n->label, doc["deviceName"].as<const char *>());

  JsonObject value = doc["value"].as<JsonObject>();
  if (!value.isNull()) {
    if (value["temperature"].is<float>() || value["temperature"].is<int>()) {
      strcpy(n->kind, "sensor");
      snprintf(n->value, sizeof n->value, "%.1fC", value["temperature"].as<float>());
      if (value["humidity"].is<float>() || value["humidity"].is<int>())
        snprintf(n->value, sizeof n->value, "%.1fC / %.0f%%",
                 value["temperature"].as<float>(), value["humidity"].as<float>());
    } else if (value["power"].is<float>() || value["power"].is<int>()) {
      strcpy(n->kind, "relay");
      snprintf(n->value, sizeof n->value, "%.1fW", value["power"].as<float>());
    } else {
      JsonDocument tmp;
      tmp.set(value);
      summarize_json(tmp, n->value, sizeof n->value);
    }
  }
  if (!strcmp(topic, "Log/")) strcpy(n->kind, "relay");
  sort_nodes();
}

/* --- public API ---------------------------------------------------------- */

void monitor_init(void) {
  memset(s_nodes, 0, sizeof s_nodes);
  s_count = 0;
}

void monitor_feed(const char *topic, const uint8_t *payload, size_t len) {
  if (!strncmp(topic, "iot/", 4)) {
    feed_iot(topic, payload, len);
  } else if (!strncmp(topic, "sensor_suhu/", 12) || !strncmp(topic, "sensor_gas_detector/", 20) ||
             !strncmp(topic, "sensor_water_tank/", 18) || !strncmp(topic, "sensor_weather/", 15) ||
             !strncmp(topic, "Log/", 4)) {
    feed_sensor_json(topic, payload, len);
  } else if (!strcmp(topic, "Sensor") || !strcmp(topic, "Status")) {
    feed_guid_value(topic, payload, len);
  }
}

void monitor_poll(void) {
  const uint32_t now = now_ms();
  for (int i = 0; i < s_count; i++) {
    if (s_nodes[i].online && (now - s_nodes[i].last_ms) > NODE_TIMEOUT_MS)
      s_nodes[i].online = false;
  }
}

int monitor_count(void) { return s_count; }
int monitor_online(void) {
  int n = 0;
  for (int i = 0; i < s_count; i++)
    if (s_nodes[i].online) n++;
  return n;
}
MonNode *monitor_node(int index) {
  if (index < 0 || index >= s_count) return NULL;
  return &s_nodes[index];
}
MonNode *monitor_find(const char *id) {
  for (int i = 0; i < s_count; i++)
    if (!strcmp(s_nodes[i].id, id)) return &s_nodes[i];
  return NULL;
}

void monitor_env(MonEnv *e) {
  memset(e, 0, sizeof *e);
  e->die_temp_c = temperatureRead();
  int adc = analogRead(PIN_LDR); /* 0..4095 with 11 dB attenuation (default) */
  int pct = (adc - 300) * 100 / (3900 - 300);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  e->light_pct = pct;
  e->light_lux = pct * pct / 40; /* very rough 0..250 lux estimate */
  e->rssi = WiFi.RSSI();
  e->uptime_s = millis() / 1000UL;
  e->heap_free = (uint32_t)ESP.getFreeHeap();
}

/* Only printable ASCII, quotes and backslashes stripped (for JSON safety). */
static void json_clean(const char *src, char *dst, size_t n) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 1 < n; i++) {
    char c = src[i];
    if (c == '"' || c == '\\') continue;
    dst[j++] = (c >= 32) ? c : ' ';
  }
  dst[j] = '\0';
}

void monitor_build_telemetry(char *buf, size_t len) {
  MonEnv e;
  monitor_env(&e);
  char dev[33], flr[24];
  json_clean(g_settings.device_id, dev, sizeof dev);
  json_clean(g_settings.floor_label, flr, sizeof flr);
  snprintf(buf, len,
           "{\"device\":\"%s\",\"role\":\"floor-monitor\",\"floor\":\"%s\","
           "\"die_temp_c\":%.1f,\"light_pct\":%d,\"lux\":%d,\"rssi\":%d,"
           "\"uptime_s\":%lu,\"heap\":%lu,\"nodes\":%d,\"online\":%d,\"ip\":\"%s\"}",
           dev, flr, e.die_temp_c, e.light_pct, e.light_lux, e.rssi,
           (unsigned long)e.uptime_s, (unsigned long)e.heap_free, monitor_count(),
           monitor_online(), WiFi.localIP().toString().c_str());
}
