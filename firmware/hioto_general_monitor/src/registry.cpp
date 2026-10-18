#include "registry.h"
#include "config.h"
#include "settings.h"
#include "board_pins.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <ctype.h>

struct RegDev {
  char guid[40];
  char shortname[20];
  char fullname[48];
  char status[8]; /* controller's last status ("0"/"1"/"") */
  bool known;
  bool on;
};

static RegDev s_devs[64];
static int s_count = 0;
static bool s_ready = false;
static uint32_t s_last_try = 0;
static char s_status[40] = "menunggu...";

static void copy_str(char *dst, size_t dst_sz, const char *src) {
  if (!src) src = "";
  strncpy(dst, src, dst_sz - 1);
  dst[dst_sz - 1] = '\0';
}

static void make_shortname(const char *name, char *out, size_t n) {
  size_t i = 0;
  while (name[i] && name[i] != '-' && i + 1 < n) {
    out[i] = name[i];
    i++;
  }
  out[i] = '\0';
  /* trim trailing spaces */
  while (i > 0 && out[i - 1] == ' ') out[--i] = '\0';
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
  http.setTimeout(10000);
  http.setConnectTimeout(4000);
  int code = http.GET();
  if (code != 200) {
    snprintf(s_status, sizeof s_status, "HTTP %d", code);
    http.end();
    return false;
  }

  char line[256];
  bool header = true;
  int n = 0;
  const int max = (int)(sizeof s_devs / sizeof s_devs[0]);
  while (read_line(http.getStream(), line, sizeof line, 2500)) {
    if (header) {
      header = false;
      continue;
    }
    if (!line[0]) continue;
    char *f[16];
    int fc = split_csv(line, f, 16, 48);
    if (fc >= 5) {
      const char *guid = f[0];
      const char *type = f[2];
      const char *name = f[3];
      const char *status = f[4];
      if (!strcmp(type, "AKTUATOR") && guid[0] && n < max) {
        copy_str(s_devs[n].guid, sizeof s_devs[n].guid, guid);
        make_shortname(name, s_devs[n].shortname, sizeof s_devs[n].shortname);
        copy_str(s_devs[n].fullname, sizeof s_devs[n].fullname, name);
        copy_str(s_devs[n].status, sizeof s_devs[n].status, status);
        /* snapshot fills the state when we haven't heard alive messages */
        if (!strcmp(status, "0")) {
          s_devs[n].known = true;
          s_devs[n].on = true;
        } else if (!strcmp(status, "1")) {
          s_devs[n].known = true;
          s_devs[n].on = false;
        }
        n++;
      }
    }
    for (int i = 0; i < fc; i++) free(f[i]);
    if (n >= max) break;
  }
  http.end();

  if (n == 0 && s_count == 0) {
    copy_str(s_status, sizeof s_status, "CSV kosong");
    return false;
  }
  s_count = n;
  s_ready = true;
  snprintf(s_status, sizeof s_status, "%d lampu", s_count);
  Serial.printf("[reg] loaded %d actuators\n", s_count);
  return true;
}

void registry_begin(void) {
  s_ready = false;
  s_count = 0;
  s_last_try = 0;
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
  return s_devs[index].shortname;
}
const char *registry_guid_at(int index) {
  if (index < 0 || index >= s_count) return NULL;
  return s_devs[index].guid;
}
const char *registry_fullname(int index) {
  if (index < 0 || index >= s_count) return NULL;
  return s_devs[index].fullname;
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

int registry_find_short(const char *shortname) {
  for (int i = 0; i < s_count; i++)
    if (!cmp_ci(s_devs[i].shortname, shortname)) return i;
  return -1;
}

int registry_find_guid(const char *guid) {
  for (int i = 0; i < s_count; i++)
    if (!strcmp(s_devs[i].guid, guid)) return i;
  return -1;
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
