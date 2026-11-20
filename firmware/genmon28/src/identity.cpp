#include "identity.h"

#include "config.h"
#include "settings.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

static bool s_registered = false;
static uint32_t s_last_try = 0;
static char s_state[48] = "belum terdaftar";

static void set_state(const char *txt) {
  snprintf(s_state, sizeof s_state, "%s", txt ? txt : "");
  Serial.printf("[id] %s\n", s_state);
}

/* MAC as "aa:bb:cc:dd:ee:ff" (the controller stores it in this shape). */
static void mac_string(char *out, size_t n) {
  uint8_t raw[6] = {0};
  WiFi.macAddress(raw);
  snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x", raw[0], raw[1], raw[2], raw[3], raw[4],
           raw[5]);
}

/* Random UUID v4 that stays stable because it is kept in NVS. */
static void generate_guid(char *out, size_t n) {
  uint32_t a = esp_random();
  uint32_t b = esp_random();
  uint32_t c = esp_random();
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  snprintf(out, n, "%08x-%04x-4%03x-%04x-%04x%04x%04x", a, (unsigned)(b >> 16),
           (unsigned)(b & 0xFFF), (unsigned)((c >> 16) & 0x3FFF) | 0x8000,
           (unsigned)((mac[3] << 8) | mac[4]), (unsigned)((mac[5] << 8) | mac[1]),
           (unsigned)((mac[2] << 8) | mac[0]));
}

void identity_begin(void) {
  if (!g_settings.guid[0]) {
    generate_guid(g_settings.guid, sizeof g_settings.guid);
    settings_save(&g_settings);
    Serial.printf("[id] GUID baru %s\n", g_settings.guid);
  } else {
    Serial.printf("[id] GUID %s\n", g_settings.guid);
  }
  s_registered = false;
  s_last_try = 0;
  snprintf(s_state, sizeof s_state, "belum terdaftar");
}

/* ------------------------------------------------------------------------- */
/*  POST /api/register {guid, mac, type, name, floor_id, room_id, ...}       */
/* ------------------------------------------------------------------------- */
bool identity_register_now(void) {
  if (WiFi.status() != WL_CONNECTED) {
    set_state("menunggu wifi");
    return false;
  }
  s_last_try = millis();

  char url[96];
  snprintf(url, sizeof url, "http://%s:%u/api/register", DEFAULT_CTRL_HOST,
           (unsigned)DEFAULT_CTRL_PORT);

  char mac[24];
  mac_string(mac, sizeof mac);
  char name[40];
  snprintf(name, sizeof name, "PANEL-%s", g_settings.device_id);

  HTTPClient http;
  WiFiClient client;
  client.setTimeout(6000);
  if (!http.begin(client, url)) {
    set_state("register: begin gagal");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);
  http.setConnectTimeout(4000);

  JsonDocument doc;
  doc["guid"] = g_settings.guid;
  doc["mac"] = mac;
  doc["type"] = "SENSOR";
  doc["name"] = name;
  doc["category"] = "SENSOR";
  doc["kind"] = "sensor";
  doc["value_type"] = "string";
  doc["floor_id"] = g_settings.floor_id;
  doc["room_id"] = g_settings.room_id;
  String body;
  serializeJson(doc, body);

  const int code = http.POST(body);
  const String resp = (code > 0) ? http.getString() : String("");
  http.end();

  if (code != 200 && code != 201) {
    char msg[32];
    snprintf(msg, sizeof msg, "register HTTP %d", code);
    set_state(msg);
    return false;
  }

  /* confirm the controller echoes our guid back */
  JsonDocument r;
  if (!deserializeJson(r, resp)) {
    const char *echo = r["guid"] | "";
    if (echo[0] && strcmp(echo, g_settings.guid)) {
      set_state("guid tidak cocok");
      return false;
    }
  }
  s_registered = true;
  set_state("terdaftar di controller");
  Serial.printf("[id] guid=%s mac=%s name=%s floor=%u room=%u\n", g_settings.guid, mac, name,
                (unsigned)g_settings.floor_id, (unsigned)g_settings.room_id);
  return true;
}

bool identity_poll(void) {
  if (s_registered) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (s_last_try && millis() - s_last_try < IDENTITY_RETRY_MS) return false;
  return identity_register_now();
}

const char *identity_guid(void) { return g_settings.guid; }
const char *identity_state(void) { return s_state; }
bool identity_registered(void) { return s_registered; }
