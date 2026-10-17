#include "registry.h"
#include "config.h"
#include "settings.h"
#include "wifi_mgr.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <ctype.h>

struct Entry {
  char guid[40];
  char name[36];
};

static Entry s_entries[64];
static int s_count = 0;
static bool s_ready = false;
static uint32_t s_last_try = 0;
static char s_status[48] = "menunggu...";

static void copy_str(char *dst, size_t dst_sz, const char *src) {
  if (!src) src = "";
  strncpy(dst, src, dst_sz - 1);
  dst[dst_sz - 1] = '\0';
}

static void upper(char *out, size_t n, const char *src) {
  size_t i = 0;
  for (; src[i] && i + 1 < n; i++) out[i] = (char)toupper((unsigned char)src[i]);
  out[i] = '\0';
}

/* The HIOTO controller derives a device floor from its NAME ("1ST", "2ND",
 * "3RD", "4TH"), falling back to the explicit floor_id for registered
 * devices. Mirror that mapping so the panel shows the right floor. */
static bool matches_floor(const char *name, int fid) {
  if (fid != 0 && fid == (int)g_settings.floor_id) return true;
  char up[48];
  upper(up, sizeof up, name);
  const char *tag = NULL;
  if (strstr(up, "1ST")) tag = "LANTAI 1";
  else if (strstr(up, "2ND")) tag = "LANTAI 2";
  else if (strstr(up, "3RD")) tag = "LANTAI 3";
  else if (strstr(up, "4TH")) tag = "LANTAI 4";
  if (!tag) return false;
  char ul[25];
  upper(ul, sizeof ul, g_settings.floor_label);
  return strcmp(tag, ul) == 0;
}

/* Read one \n-terminated line from the stream; returns false on timeout/EOF. */
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

/* Split one CSV line into fields; returns the field count ("" handled). */
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
  if (wifi_mgr_state() != WIFI_ST_OK) return false;

  char url[96];
  snprintf(url, sizeof url, "http://%s:%u/api/devices/export.csv", g_settings.ctrl_host,
           (unsigned)g_settings.ctrl_port);
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

  /* CSV columns: guid,mac,type,name,status,status_device,version,minor,
   *              category,quantity,room_id,floor_id,x,y,last_seen */
  char line[256];
  bool header = true;
  int n = 0;
  const int max = (int)(sizeof s_entries / sizeof s_entries[0]);
  while (read_line(http.getStream(), line, sizeof line, 2500)) {
    if (header) { /* skip the header row */
      header = false;
      continue;
    }
    if (!line[0]) continue;
    char *f[16];
    int fc = split_csv(line, f, 16, 48);
    if (fc >= 12) {
      const char *guid = f[0];
      const char *name = f[3];
      int fid = atoi(f[11]);
      if (guid[0] && matches_floor(name, fid) && n < max) {
        copy_str(s_entries[n].guid, sizeof s_entries[n].guid, guid);
        copy_str(s_entries[n].name, sizeof s_entries[n].name, name);
        n++;
      }
    }
    for (int i = 0; i < fc; i++) free(f[i]);
    if (n >= max) break;
  }
  http.end();

  if (n == 0 && s_count == 0) {
    /* Couldn't read the body at all: keep trying, report it. */
    copy_str(s_status, sizeof s_status, "CSV kosong");
    return false;
  }
  s_count = n;
  s_ready = true;
  snprintf(s_status, sizeof s_status, "%d perangkat lantai", s_count);
  Serial.printf("[reg] loaded %d devices for floor '%s'\n", s_count,
                g_settings.floor_label);
  return true;
}

void registry_begin(void) {
  s_ready = false;
  s_count = 0;
  s_last_try = 0;
  copy_str(s_status, sizeof s_status, "menunggu...");
}

bool registry_poll(void) {
  if (s_ready) return false;
  if (wifi_mgr_state() != WIFI_ST_OK) return false;
  if (s_last_try && (millis() - s_last_try) < 15000) return false;
  s_last_try = millis();
  return fetch_registry();
}

bool registry_ready(void) { return s_ready; }

const char *registry_name(const char *guid) {
  for (int i = 0; i < s_count; i++)
    if (!strcmp(s_entries[i].guid, guid)) return s_entries[i].name;
  return NULL;
}

bool registry_contains(const char *guid) {
  for (int i = 0; i < s_count; i++)
    if (!strcmp(s_entries[i].guid, guid)) return true;
  return false;
}

int registry_count(void) { return s_count; }

const char *registry_status(void) { return s_status; }
