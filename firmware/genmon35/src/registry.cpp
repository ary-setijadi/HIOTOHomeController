#include "registry.h"
#include "config.h"
#include "settings.h"
#include "board_pins.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <ctype.h>

/* The HIOTO controller export currently holds ~81 AKTUATOR rows; keep headroom. */
#define REG_MAX_DEVS 128
#define REG_LABEL_LEN 24
#define REG_NAME_LEN 52
#define REG_FLOOR_LEN 20
#define REG_ROOM_LEN 28

struct RegDev {
  char guid[40];
  char label[REG_LABEL_LEN]; /* compact id: panel button + matching ("LAMP.14") */
  char name[REG_NAME_LEN];   /* full registry name */
  char floor[REG_FLOOR_LEN]; /* floor text from the name ("1ST.FLOOR") */
  char room[REG_ROOM_LEN];   /* room text from the name ("LIVING ROOM") */
  char status[8];            /* controller's last status ("0"/"1"/"") */
  uint8_t floor_id;          /* controller floor id, 0 when unset */
  uint8_t room_id;           /* controller room id, 0 when unset */
  bool known;
  bool on;
};

static RegDev s_devs[REG_MAX_DEVS];
static uint8_t s_used[REG_MAX_DEVS];
static int s_count = 0;      /* live devices */
static int s_prev_count = 0; /* devices known before the current refresh */
static bool s_ready = false;
static uint32_t s_last_try = 0;
static char s_status[40] = "menunggu...";

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

static int cmp_ci(const char *a, const char *b) {
  while (*a && *b) {
    int ca = toupper((unsigned char)*a);
    int cb = toupper((unsigned char)*b);
    if (ca != cb) return ca - cb;
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

/* Drop the trailing '}' some registry names carry, trim spaces. */
static void clean_name(const char *src, char *out, size_t n) {
  size_t i = 0;
  while (src[i] && i + 1 < n) {
    out[i] = src[i];
    i++;
  }
  while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '}')) i--;
  out[i] = '\0';
}

static int split_parts(const char *name, char parts[][28], int max) {
  int n = 0;
  size_t fi = 0;
  for (size_t p = 0;; p++) {
    char c = name[p];
    if (c == '-' || c == '\0') {
      if (n < max) {
        parts[n][fi] = '\0';
        n++;
      }
      fi = 0;
      if (c == '\0') break;
    } else if (fi + 1 < 28 && n < max) {
      parts[n][fi++] = c;
    }
  }
  return n;
}

/* ---------------------------------------------------------------------------
 *  Compact label used on the panel button (short and distinct).
 *
 *   LAMP.12-1ST.FLOOR-LIVING ROOM-251-02  -> LAMP.12
 *   HIOTO-SMARTSTEKER-16                  -> SMARTSTEKER-16
 *   LSKK-HIOTO-DO-270-01                  -> DO-270-01
 *   LSKK-IWK-SAKLAR-79                    -> IWK-SAKLAR-79
 *   lamp-LSKK-HIOTO-DO-01-TEST            -> DO-01-TEST
 * ------------------------------------------------------------------------- */
static void make_label(const char *name, char *out, size_t n) {
  char parts[8][28];
  int np = split_parts(name, parts, 8);
  out[0] = '\0';
  if (np == 0) {
    copy_str(out, n, name);
    return;
  }

  /* Lamp family already starts with a usable id. */
  if (!strncasecmp(parts[0], "LAMP.", 5)) {
    copy_str(out, n, parts[0]);
    return;
  }

  /* Skip vendor / type prefixes that are identical on many devices. */
  int s = 0;
  while (s < np && (ci_eq(parts[s], "HIOTO") || ci_eq(parts[s], "LSKK") ||
                    ci_eq(parts[s], "lamp") || ci_eq(parts[s], "LAMP")))
    s++;
  if (s >= np) s = 0;

  char buf[64];
  buf[0] = '\0';
  for (int i = s; i < np; i++) {
    if (i > s && strlen(buf) + 1 < sizeof buf) strcat(buf, "-");
    strncat(buf, parts[i], sizeof buf - strlen(buf) - 1);
  }

  /* Keep it inside the panel label budget; fall back to first + last part. */
  if (strlen(buf) > 15 && np - s >= 2) {
    snprintf(buf, sizeof buf, "%s-%s", parts[s], parts[np - 1]);
  }
  copy_str(out, n, buf);
}

/* Floor / room text from the registry name.
 * LAMP.14-1ST.CONNECTING SPACE-252-02 -> floor "1ST", room "CONNECTING SPACE".
 * Other families carry no location in their name. */
static void make_location(const char *name, char *floor_out, size_t fn, char *room_out,
                          size_t rn) {
  floor_out[0] = '\0';
  room_out[0] = '\0';
  char parts[8][28];
  int np = split_parts(name, parts, 8);
  if (np < 3) return;
  if (strncasecmp(parts[0], "LAMP.", 5)) return;
  copy_str(floor_out, fn, parts[1]);
  copy_str(room_out, rn, parts[2]);
}

/* Append #2, #3, ... to labels that collide (same name on several relays).
 * Run after sort_devs() so equal labels are adjacent. */
static void uniquify_labels(void) {
  char prev[REG_LABEL_LEN] = "";
  int run = 0;
  for (int i = 0; i < s_count; i++) {
    if (i > 0 && cmp_ci(prev, s_devs[i].label) == 0) run++;
    else run = 0;
    copy_str(prev, sizeof prev, s_devs[i].label);
    if (run > 0) {
      char base[REG_LABEL_LEN];
      copy_str(base, sizeof base, s_devs[i].label);
      snprintf(s_devs[i].label, sizeof s_devs[i].label, "%.18s#%d", base, run + 1);
    }
  }
}

static void sort_devs(void) {
  for (int i = 1; i < s_count; i++) {
    RegDev tmp = s_devs[i];
    int j = i - 1;
    while (j >= 0) {
      int c = cmp_ci(s_devs[j].label, tmp.label);
      if (c == 0) c = cmp_ci(s_devs[j].name, tmp.name);
      if (c <= 0) break;
      s_devs[j + 1] = s_devs[j];
      j--;
    }
    s_devs[j + 1] = tmp;
  }
}

/* Read one \n-terminated line from the stream; false on timeout/EOF. */
static bool read_line(Stream &s, char *buf, size_t max, uint32_t timeout_ms) {
  size_t i = 0;
  uint32_t t0 = millis();
  while (i + 1 < max) {
    if (s.available() > 0) {
      int c = s.read();
      if (c < 0) return false;
      if (c == '\n') {
        buf[i] = '\0';
        return true;
      }
      if (c != '\r') buf[i++] = (char)c;
      t0 = millis();
    } else if (millis() - t0 > timeout_ms) {
      return false;
    } else {
      delay(1);
    }
  }
  buf[i] = '\0';
  return true;
}

/* Split one CSV line into fields (double-quote aware); caller frees fields. */
static int split_csv(char *line, char *fields[], int max_fields, size_t field_sz) {
  int n = 0;
  size_t fi = 0;
  bool in_quotes = false;
  fields[0] = (char *)calloc(field_sz, 1);
  if (!fields[0]) return 0;
  for (size_t p = 0; line[p]; p++) {
    char c = line[p];
    if (in_quotes) {
      if (c == '"') {
        if (line[p + 1] == '"') {
          if (fi + 1 < field_sz) fields[n][fi++] = '"';
          p++;
        } else {
          in_quotes = false;
        }
      } else if (fi + 1 < field_sz) {
        fields[n][fi++] = c;
      }
    } else if (c == '"') {
      in_quotes = true;
    } else if (c == ',') {
      fields[n][fi] = '\0';
      if (++n >= max_fields) { n = max_fields; break; }
      fi = 0;
      fields[n] = (char *)calloc(field_sz, 1);
      if (!fields[n]) return 0;
    } else if (fi + 1 < field_sz) {
      fields[n][fi++] = c;
    }
  }
  fields[n][fi] = '\0';
  return n + 1;
}

/* Find a device already known from the previous refresh (by guid), so the
 * ON/OFF state learned over MQTT survives. Searches every previous entry,
 * not just the ones already re-seen in this pass. */
static int find_prev_by_guid(const char *guid) {
  for (int i = 0; i < s_prev_count; i++)
    if (!strcmp(s_devs[i].guid, guid)) return i;
  return -1;
}

static bool fetch_registry(void) {
  if (WiFi.status() != WL_CONNECTED) return false;

  char url[96];
  snprintf(url, sizeof url, "http://%s:%u/api/devices/export.csv", DEFAULT_CTRL_HOST,
           (unsigned)DEFAULT_CTRL_PORT);
  Serial.printf("[reg] GET %s\n", url);

  HTTPClient http;
  WiFiClient client;
  client.setTimeout(6000);
  if (!http.begin(client, url)) {
    copy_str(s_status, sizeof s_status, "begin gagal");
    return false;
  }
  http.setTimeout(5000);
  http.setConnectTimeout(2500);
  int code = http.GET();
  if (code != 200) {
    snprintf(s_status, sizeof s_status, "HTTP %d", code);
    http.end();
    return false;
  }

  s_prev_count = s_count;
  memset(s_used, 0, sizeof s_used);
  int added = 0;

  char line[256];
  char namebuf[REG_NAME_LEN];
  bool header = true;
  while (read_line(http.getStream(), line, sizeof line, 2500)) {
    if (header) {
      header = false;
      continue;
    }
    if (!line[0]) continue;
    char *f[16];
    int fc = split_csv(line, f, 16, 64);
    if (fc >= 5) {
      const char *guid = f[0];
      const char *type = f[2];
      const char *name = f[3];
      const char *status = f[4];
      /* CSV: ...,quantity,room_id,floor_id,x_position,y_position,last_seen */
      const int room_id = (fc > 10) ? atoi(f[10]) : 0;
      const int floor_id = (fc > 11) ? atoi(f[11]) : 0;
      if (!strcmp(type, "AKTUATOR") && guid[0]) {
        clean_name(name, namebuf, sizeof namebuf);
        int idx = find_prev_by_guid(guid);
        if (idx < 0 && s_count < REG_MAX_DEVS) {
          idx = s_count++;
          memset(&s_devs[idx], 0, sizeof s_devs[idx]);
          copy_str(s_devs[idx].guid, sizeof s_devs[idx].guid, guid);
          added++;
        }
        if (idx >= 0) {
          RegDev *d = &s_devs[idx];
          s_used[idx] = 1;
          copy_str(d->guid, sizeof d->guid, guid);
          copy_str(d->name, sizeof d->name, namebuf);
          make_label(namebuf, d->label, sizeof d->label);
          make_location(namebuf, d->floor, sizeof d->floor, d->room, sizeof d->room);
          d->floor_id = (floor_id > 0 && floor_id <= 255) ? (uint8_t)floor_id : 0;
          d->room_id = (room_id > 0 && room_id <= 255) ? (uint8_t)room_id : 0;
          copy_str(d->status, sizeof d->status, status);
          /* the export is the fallback truth; live MQTT traffic wins later */
          if (!strcmp(status, "0")) {
            d->known = true;
            d->on = true;
          } else if (!strcmp(status, "1")) {
            d->known = true;
            d->on = false;
          }
        }
      }
    }
    for (int i = 0; i < fc; i++) free(f[i]);
  }
  http.end();

  /* Drop devices that vanished from the export. */
  int keep = 0;
  for (int i = 0; i < s_count; i++) {
    if (!s_used[i]) continue;
    if (keep != i) s_devs[keep] = s_devs[i];
    keep++;
  }
  s_count = keep;
  s_prev_count = 0;

  if (s_count == 0) {
    copy_str(s_status, sizeof s_status, "CSV kosong");
    return false;
  }

  sort_devs();      /* group duplicates so the numbering pass sees them */
  uniquify_labels();
  sort_devs();      /* final order with the #n suffixes applied */
  s_ready = true;
  snprintf(s_status, sizeof s_status, "%d lampu", s_count);
  Serial.printf("[reg] loaded %d actuators (%d baru)\n", s_count, added);
  return true;
}

void registry_begin(void) {
  s_ready = false;
  s_count = 0;
  s_prev_count = 0;
  s_last_try = 0;
  memset(s_used, 0, sizeof s_used);
  copy_str(s_status, sizeof s_status, "menunggu...");
}

bool registry_poll(void) {
  if (WiFi.status() != WL_CONNECTED) return false;
  const uint32_t interval = s_ready ? REGISTRY_REFRESH_MS : 15000UL;
  if (s_last_try && (millis() - s_last_try) < interval) return false;
  s_last_try = millis();
  return fetch_registry();
}

bool registry_ready(void) { return s_ready; }

int registry_count(void) { return s_count; }

const char *registry_shortname(int index) {
  if (index < 0 || index >= s_count) return NULL;
  return s_devs[index].label;
}
const char *registry_guid_at(int index) {
  if (index < 0 || index >= s_count) return NULL;
  return s_devs[index].guid;
}
const char *registry_fullname(int index) {
  if (index < 0 || index >= s_count) return NULL;
  return s_devs[index].name;
}
const char *registry_floor_at(int index) {
  if (index < 0 || index >= s_count) return NULL;
  return s_devs[index].floor;
}
const char *registry_room_at(int index) {
  if (index < 0 || index >= s_count) return NULL;
  return s_devs[index].room;
}

int registry_find_short(const char *shortname) {
  if (!shortname || !shortname[0]) return -1;
  for (int i = 0; i < s_count; i++)
    if (!cmp_ci(s_devs[i].label, shortname)) return i;
  /* tolerate a spaced form such as "LAMP.14 " or "lamp 14" */
  for (int i = 0; i < s_count; i++)
    if (!strncmp(s_devs[i].name, shortname, strlen(shortname))) return i;
  return -1;
}

int registry_find_guid(const char *guid) {
  if (!guid || !guid[0]) return -1;
  for (int i = 0; i < s_count; i++)
    if (!strcmp(s_devs[i].guid, guid)) return i;
  return -1;
}

int registry_resolve(const char *guid, const char *label) {
  int ri = registry_find_guid(guid);
  if (ri >= 0) return ri;
  return registry_find_short(label);
}

/* ------------------------------------------------------------------------- */
/*  Floor / room matching                                                    */
/* ------------------------------------------------------------------------- */
/* Uppercase alphanumeric only: "Living Room" -> "LIVINGROOM". */
static void norm_text(const char *src, char *out, size_t n) {
  size_t o = 0;
  for (size_t i = 0; src[i] && o + 1 < n; i++) {
    unsigned char c = (unsigned char)src[i];
    if (isalnum(c)) out[o++] = (char)toupper(c);
  }
  out[o] = '\0';
}

/* First run of digits: "Lantai 1" -> "1", "1ST.FLOOR" -> "1". */
static void first_number(const char *src, char *out, size_t n) {
  size_t o = 0;
  bool started = false;
  for (size_t i = 0; src[i] && o + 1 < n; i++) {
    if (isdigit((unsigned char)src[i])) {
      out[o++] = src[i];
      started = true;
    } else if (started) {
      break;
    }
  }
  out[o] = '\0';
}

/* Same place name, tolerating typos like CONNECTION vs CONNECTING SPACE. */
static bool text_like(const char *a, const char *b) {
  char na[80], nb[80];
  norm_text(a, na, sizeof na);
  norm_text(b, nb, sizeof nb);
  if (!na[0] || !nb[0]) return false;
  if (!strcmp(na, nb)) return true;
  size_t i = 0;
  while (na[i] && nb[i] && na[i] == nb[i]) i++;
  return i >= 5;
}

static bool floor_like(const char *dev_floor, const char *want) {
  char dn[24], wn[24];
  first_number(dev_floor, dn, sizeof dn);
  first_number(want, wn, sizeof wn);
  if (dn[0] && wn[0]) return !strcmp(dn, wn);
  if (!dn[0]) return false;
  return text_like(dev_floor, want);
}

bool registry_matches_scope(int index, uint8_t scope, uint8_t floor_id, uint8_t room_id,
                            const char *floor_label, const char *room_label) {
  if (index < 0 || index >= s_count) return false;
  if (scope >= 2) return true; /* every actuator */

  const RegDev *d = &s_devs[index];

  /* Controller ids are authoritative when the device carries them ... */
  const bool id_floor = floor_id && d->floor_id == floor_id;
  const bool id_room = room_id && d->room_id == room_id;
  if (id_floor && (scope == 1 || id_room)) return true;

  /* ... otherwise fall back to the floor/room text inside the name. */
  if (!d->floor[0] && !d->room[0]) return false; /* location unknown */
  if (floor_label && floor_label[0] && !floor_like(d->floor, floor_label)) return false;
  if (scope == 0 && room_label && room_label[0] && !text_like(d->room, room_label))
    return false;
  return true;
}

bool registry_state_known(int index) {
  if (index < 0 || index >= s_count) return false;
  return s_devs[index].known;
}
bool registry_state_on(int index) {
  if (index < 0 || index >= s_count) return false;
  return s_devs[index].on;
}
void registry_set_state(int index, bool on) {
  if (index < 0 || index >= s_count) return;
  s_devs[index].known = true;
  s_devs[index].on = on;
}

const char *registry_status(void) { return s_status; }
