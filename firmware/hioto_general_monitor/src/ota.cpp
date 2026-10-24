#include "ota.h"

#include "config.h"
#include "settings.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

static char s_status[72] = "siap";
static uint32_t s_last_check = 0;
static bool s_request = false;
static bool s_busy = false;

const char *ota_version(void) { return FW_VERSION; }
const char *ota_status_text(void) { return s_status; }

static void set_status(const char *text) {
  snprintf(s_status, sizeof s_status, "%s", text ? text : "");
  Serial.printf("[ota] %s\n", s_status);
}

static void set_statusf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_status, sizeof s_status, fmt, ap);
  va_end(ap);
  Serial.printf("[ota] %s\n", s_status);
}

/* ------------------------------------------------------------------------- */
/*  POST /api/report                                                         */
/* ------------------------------------------------------------------------- */
static void report(const char *status, const char *from, const char *to,
                   const char *message) {
  if (WiFi.status() != WL_CONNECTED) return;
  char url[96];
  snprintf(url, sizeof url, "%s/api/report", OTA_BASE);

  HTTPClient http;
  WiFiClient client;
  client.setTimeout(5000);
  if (!http.begin(client, url)) return;
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["device_type"] = FW_DEVICE_TYPE;
  doc["guid"] = g_settings.device_id;
  doc["status"] = status;
  doc["from_version"] = from;
  doc["to_version"] = to;
  doc["message"] = message;
  String body;
  serializeJson(doc, body);
  const int code = http.POST(body);
  Serial.printf("[ota] report %s -> HTTP %d\n", status, code);
  http.end();
}

/* ------------------------------------------------------------------------- */
/*  GET /api/latest?device=<type>&guid=<id>                                  */
/* ------------------------------------------------------------------------- */
static bool fetch_manifest(char *ver, size_t ver_n, char *url, size_t url_n, int *size) {
  char req[128];
  snprintf(req, sizeof req, "%s/api/latest?device=%s&guid=%s", OTA_BASE, FW_DEVICE_TYPE,
           g_settings.device_id);

  HTTPClient http;
  WiFiClient client;
  client.setTimeout(8000);
  if (!http.begin(client, req)) {
    set_status("manifest: begin gagal");
    return false;
  }
  http.setTimeout(8000);
  http.setConnectTimeout(5000);
  const int code = http.GET();
  if (code != 200) {
    set_statusf("manifest HTTP %d", code);
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    set_status("manifest: JSON invalid");
    return false;
  }
  const char *v = doc["latest_version"] | "";
  const char *u = doc["url"] | "";
  if (!v[0] || !u[0]) {
    set_status("manifest: kosong");
    return false;
  }
  snprintf(ver, ver_n, "%s", v);
  snprintf(url, url_n, "%s", u);
  if (size) *size = doc["size"] | 0;
  return true;
}

/* ------------------------------------------------------------------------- */
/*  Stream a firmware URL into the inactive OTA slot (ESP-IDF API directly,   */
/*  so the slot selection and boot switch are explicit and logged).          */
/* ------------------------------------------------------------------------- */
bool ota_flash_url(const char *url) {
  if (WiFi.status() != WL_CONNECTED) {
    set_status("wifi belum tersambung");
    return false;
  }
  if (!url || !url[0]) {
    set_status("url kosong");
    return false;
  }

  Serial.printf("[ota] unduh %s\n", url);
  set_status("mengunduh firmware...");

  HTTPClient http;
  WiFiClient client;
  client.setTimeout(15000);
  if (!http.begin(client, url)) {
    set_status("unduh: begin gagal");
    return false;
  }
  http.setTimeout(20000);
  http.setConnectTimeout(5000);
  const int code = http.GET();
  if (code != 200) {
    set_statusf("unduh HTTP %d", code);
    http.end();
    return false;
  }

  const int len = http.getSize();
  if (len <= 0) {
    set_status("unduh: ukuran tidak diketahui");
    http.end();
    return false;
  }

  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  Serial.printf("[ota] boot=%s run=%s next=%s slot=%u size=%d\n",
                boot ? boot->label : "?", run ? run->label : "?",
                next ? next->label : "?", (unsigned)(next ? next->size : 0), len);
  if (!next) {
    set_status("tidak ada slot OTA");
    http.end();
    return false;
  }
  if ((size_t)len > next->size) {
    set_statusf("slot %s terlalu kecil", next->label);
    http.end();
    return false;
  }

  s_busy = true;
  esp_ota_handle_t handle = 0;
  esp_err_t err = esp_ota_begin(next, (size_t)len, &handle);
  if (err != ESP_OK) {
    set_statusf("ota_begin err=%d", (int)err);
    http.end();
    s_busy = false;
    return false;
  }

  Serial.printf("[ota] menerima %d byte -> %s\n", len, next->label);
  static uint8_t buf[2048];
  WiFiClient *stream = http.getStreamPtr();
  int remaining = len;
  bool ok = true;
  while (remaining > 0) {
    const int want = remaining > (int)sizeof buf ? (int)sizeof buf : remaining;
    const int got = stream->readBytes(buf, want);
    if (got <= 0) {
      ok = false;
      break;
    }
    err = esp_ota_write(handle, buf, (size_t)got);
    if (err != ESP_OK) {
      ok = false;
      break;
    }
    remaining -= got;
  }
  http.end();

  if (!ok || remaining != 0) {
    set_statusf("tulis gagal sisa=%d", remaining);
    esp_ota_abort(handle);
    s_busy = false;
    return false;
  }
  err = esp_ota_end(handle);
  if (err != ESP_OK) {
    set_statusf("ota_end err=%d", (int)err);
    s_busy = false;
    return false;
  }
  err = esp_ota_set_boot_partition(next);
  if (err != ESP_OK) {
    set_statusf("set_boot err=%d", (int)err);
    s_busy = false;
    return false;
  }
  const esp_partition_t *nb = esp_ota_get_boot_partition();
  Serial.printf("[ota] boot partition sekarang %s\n", nb ? nb->label : "?");
  set_status("firmware terpasang, reboot...");
  s_busy = false;
  return true;
}

/* ------------------------------------------------------------------------- */
bool ota_check_now(void) {
  if (WiFi.status() != WL_CONNECTED) {
    set_status("wifi belum tersambung");
    return false;
  }
  if (s_busy) return false;
  s_last_check = millis();

  char ver[24] = "";
  char url[160] = "";
  if (!fetch_manifest(ver, sizeof ver, url, sizeof url, nullptr)) return false;

  if (!strcmp(ver, FW_VERSION)) {
    set_statusf("terbaru sudah %s", FW_VERSION);
    return false;
  }

  Serial.printf("[ota] versi %s -> %s\n", FW_VERSION, ver);
  if (!ota_flash_url(url)) {
    report("failed", FW_VERSION, ver, s_status);
    return false;
  }
  report("success", FW_VERSION, ver, "flash ok");
  delay(400);
  Serial.println("[ota] rebooting into the new firmware");
  ESP.restart();
  return true;
}

void ota_request_check(void) { s_request = true; }

void ota_begin(void) {
  s_last_check = millis();
  Serial.printf("[ota] fw=%s type=%s chip=%s server=%s\n", FW_VERSION, FW_DEVICE_TYPE,
                FW_PANEL_CHIP, OTA_BASE);
}

void ota_poll(void) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (s_request) {
    s_request = false;
    ota_check_now();
    return;
  }
  if (!g_settings.ota_auto) return;
  if (millis() - s_last_check < OTA_CHECK_MS) return;
  s_last_check = millis();
  ota_check_now();
}
