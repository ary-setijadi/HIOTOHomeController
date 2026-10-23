#include "catalog.h"

#include "config.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

#define CAT_REFRESH_MS 300000UL /* 5 min */

struct FloorItem {
  uint8_t id;
  char name[24];
};

struct RoomItem {
  uint8_t id;
  uint8_t floor_id;
  char name[24];
};

static FloorItem s_floors[CAT_MAX_FLOORS];
static RoomItem s_rooms[CAT_MAX_ROOMS];
static int s_floor_count = 0;
static int s_room_count = 0;
static bool s_ready = false;
static uint32_t s_last_try = 0;

static void copy_str(char *dst, size_t dst_sz, const char *src) {
  if (!src) src = "";
  strncpy(dst, src, dst_sz - 1);
  dst[dst_sz - 1] = '\0';
}

static bool ci_eq(const char *a, const char *b) {
  while (*a && *b) {
    if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

/* Fetch one endpoint into `body`; false on any failure. */
static bool http_get(const char *path, String &body) {
  char url[96];
  snprintf(url, sizeof url, "http://%s:%u%s", DEFAULT_CTRL_HOST,
           (unsigned)DEFAULT_CTRL_PORT, path);
  HTTPClient http;
  WiFiClient client;
  client.setTimeout(6000);
  if (!http.begin(client, url)) return false;
  http.setTimeout(8000);
  http.setConnectTimeout(4000);
  const int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  body = http.getString();
  http.end();
  return body.length() > 0;
}

static int parse_floors(const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return -1;
  if (!doc.is<JsonArray>()) return -1;
  int n = 0;
  for (JsonObject o : doc.as<JsonArray>()) {
    if (n >= CAT_MAX_FLOORS) break;
    const int id = o["id"] | 0;
    const char *name = o["name"] | "";
    if (id <= 0 || id > 255 || !name[0]) continue;
    s_floors[n].id = (uint8_t)id;
    copy_str(s_floors[n].name, sizeof s_floors[n].name, name);
    n++;
  }
  return n;
}

static int parse_rooms(const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return -1;
  if (!doc.is<JsonArray>()) return -1;
  int n = 0;
  for (JsonObject o : doc.as<JsonArray>()) {
    if (n >= CAT_MAX_ROOMS) break;
    const int id = o["id"] | 0;
    const int fid = o["floor_id"] | 0;
    const char *name = o["name"] | "";
    if (id <= 0 || id > 255 || !name[0]) continue;
    s_rooms[n].id = (uint8_t)id;
    s_rooms[n].floor_id = (fid > 0 && fid <= 255) ? (uint8_t)fid : 0;
    copy_str(s_rooms[n].name, sizeof s_rooms[n].name, name);
    n++;
  }
  return n;
}

/* Cheap fingerprint of the parsed catalog, used to detect real changes. */
static uint32_t s_hash = 0;

static uint32_t hash_state(void) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < s_floor_count; i++) {
    h = (h ^ s_floors[i].id) * 16777619u;
    for (const char *p = s_floors[i].name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  }
  for (int i = 0; i < s_room_count; i++) {
    h = (h ^ s_rooms[i].id) * 16777619u;
    h = (h ^ s_rooms[i].floor_id) * 16777619u;
    for (const char *p = s_rooms[i].name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  }
  return h;
}

void catalog_begin(void) {
  s_floor_count = 0;
  s_room_count = 0;
  s_ready = false;
  s_last_try = 0;
  s_hash = 0;
}

bool catalog_poll(void) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (s_last_try && (millis() - s_last_try) < CAT_REFRESH_MS) return false;
  s_last_try = millis();

  String body;
  int floors = -1;
  int rooms = -1;

  if (http_get("/api/floors", body)) {
    floors = parse_floors(body);
    body = "";
  }
  if (http_get("/api/rooms", body)) {
    rooms = parse_rooms(body);
  }

  if (floors < 0 && rooms < 0) {
    Serial.println("[cat] controller API tidak terjangkau");
    return false;
  }
  if (floors >= 0) s_floor_count = floors;
  if (rooms >= 0) s_room_count = rooms;
  s_ready = true;

  const uint32_t h = hash_state();
  const bool diff = (h != s_hash);
  s_hash = h;
  Serial.printf("[cat] %d lantai, %d ruang%s\n", s_floor_count, s_room_count,
                diff ? " (berubah)" : "");
  return diff;
}

bool catalog_ready(void) { return s_ready; }

int catalog_floor_count(void) { return s_floor_count; }

const char *catalog_floor_name(int idx) {
  if (idx < 0 || idx >= s_floor_count) return "";
  return s_floors[idx].name;
}

uint8_t catalog_floor_id(int idx) {
  if (idx < 0 || idx >= s_floor_count) return 0;
  return s_floors[idx].id;
}

int catalog_room_count(uint8_t floor_id) {
  if (!floor_id) return s_room_count;
  int n = 0;
  for (int i = 0; i < s_room_count; i++)
    if (s_rooms[i].floor_id == floor_id) n++;
  return n;
}

const char *catalog_room_name(uint8_t floor_id, int idx) {
  int seen = 0;
  for (int i = 0; i < s_room_count; i++) {
    if (floor_id && s_rooms[i].floor_id != floor_id) continue;
    if (seen == idx) return s_rooms[i].name;
    seen++;
  }
  return "";
}

uint8_t catalog_room_id(uint8_t floor_id, int idx) {
  int seen = 0;
  for (int i = 0; i < s_room_count; i++) {
    if (floor_id && s_rooms[i].floor_id != floor_id) continue;
    if (seen == idx) return s_rooms[i].id;
    seen++;
  }
  return 0;
}

/* Position of a room inside its floor's filtered list (-1 when outside). */
static int room_pos(uint8_t floor_id, int global_idx) {
  if (global_idx < 0 || global_idx >= s_room_count) return -1;
  if (floor_id && s_rooms[global_idx].floor_id != floor_id) return -1;
  int pos = 0;
  for (int i = 0; i < global_idx; i++) {
    if (floor_id && s_rooms[i].floor_id != floor_id) continue;
    pos++;
  }
  return pos;
}

int catalog_find_floor(uint8_t id, const char *name) {
  for (int i = 0; i < s_floor_count; i++) {
    if (id && s_floors[i].id == id) return i;
    if (!id && name && name[0] && ci_eq(s_floors[i].name, name)) return i;
  }
  if (name && name[0]) /* id stale or unset -> match by name */
    for (int i = 0; i < s_floor_count; i++)
      if (ci_eq(s_floors[i].name, name)) return i;
  return -1;
}

/* Returns the position within the (floor_id) filtered room list. */
int catalog_find_room(uint8_t floor_id, uint8_t id, const char *name) {
  for (int i = 0; i < s_room_count; i++) {
    if (floor_id && s_rooms[i].floor_id != floor_id) continue;
    if (id && s_rooms[i].id == id) return room_pos(floor_id, i);
    if (!id && name && name[0] && ci_eq(s_rooms[i].name, name))
      return room_pos(floor_id, i);
  }
  if (name && name[0])
    for (int i = 0; i < s_room_count; i++) {
      if (floor_id && s_rooms[i].floor_id != floor_id) continue;
      if (ci_eq(s_rooms[i].name, name)) return room_pos(floor_id, i);
    }
  return -1;
}
