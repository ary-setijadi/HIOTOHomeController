/**
 * HIOTO General Monitor — lamp switch panel (v1.0)
 * ---------------------------------------------------------------------------
 *  Board: Sunton ESP32-3248S035 (3.5" CYD)
 *    - ESP32-D0WD-V3, 4 MB flash, no PSRAM
 *    - ST7796 320x480 native panel, driven as 480x320 landscape
 *    - XPT2046 resistive touch (shared SPI bus, CS=GPIO33)
 *    - CH340 @ COM3
 *
 *  A simple lamp switch panel:
 *    - title [Floor] · [Room]
 *    - 2 x 4 big buttons (lamp icon + short id); tap = toggle lamp
 *    - "Setelan" pill: WiFi, Floor/Room, per-button lamp + icon assignment
 */
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ArduinoOTA.h>

#include "board_pins.h"
#include "config.h"
#include "settings.h"
#include "registry.h"
#include "catalog.h"
#include "identity.h"
#include "mqtt_mgr.h"
#include "theme.h"
#include "backlight.h"
#include "ota.h"
#include "ui.h"

static TFT_eSPI s_tft;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;

static const uint32_t kDrawLines = 20;
static lv_color_t *s_buf_a = nullptr;
static lv_color_t *s_buf_b = nullptr;

static bool s_btn_prev = false;
static bool s_btn_armed = false;
static uint32_t s_btn_pressed_at = 0;
static bool s_btn_long_done = false;
static uint32_t s_last_ui = 0;
static bool s_wifi_started = false;

/* ------------------------------------------------------------------------- */
/*  LVGL flush                                                               */
/* ------------------------------------------------------------------------- */
static void lv_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  const uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
  const uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
  s_tft.startWrite();
  s_tft.setAddrWindow(area->x1, area->y1, w, h);
  s_tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  s_tft.endWrite();
  lv_disp_flush_ready(disp);
}

/* ------------------------------------------------------------------------- */
/*  Touch                                                                    */
/* ------------------------------------------------------------------------- */
static void indev_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  (void)drv;
  uint16_t x = 0, y = 0;
  if (s_tft.getTouch(&x, &y, 400)) {
    data->point.x = (lv_coord_t)x;
    data->point.y = (lv_coord_t)y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

/* ------------------------------------------------------------------------- */
/*  Wi-Fi                                                                    */
/* ------------------------------------------------------------------------- */
void wifi_apply(const char *ssid, const char *pass) {
  if (!ssid || !ssid[0]) return;
  Serial.printf("[wifi] applying '%s'\n", ssid);
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  if (pass && pass[0]) WiFi.begin(ssid, pass);
  else WiFi.begin(ssid);
  s_wifi_started = true;
}

bool wifi_ok(void) { return WiFi.status() == WL_CONNECTED; }

/* ------------------------------------------------------------------------- */
/*  OTA (flash over Wi-Fi) — avoids the manual BOOT + replug dance           */
/* ------------------------------------------------------------------------- */
static bool s_ota_started = false;

static void ard_ota_begin(void) {
  ArduinoOTA.setHostname(g_settings.device_id);
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.setPort(3232);
  ArduinoOTA.onStart([]() {
    Serial.println("[ota] update starting");
    backlight_set(60);
  });
  ArduinoOTA.onEnd([]() { Serial.println("\n[ota] done, rebooting"); });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static int last = -1;
    const int pct = total ? (int)(done * 100 / total) : 0;
    if (pct / 10 != last / 10) {
      last = pct;
      Serial.printf("[ota] %d%%\n", pct);
    }
  });
  ArduinoOTA.onError([](ota_error_t err) { Serial.printf("[ota] error %u\n", err); });
  ArduinoOTA.begin();
  s_ota_started = true;
  Serial.printf("[ota] ready on %s:3232\n", WiFi.localIP().toString().c_str());
}

const char *wifi_state_text(void) {
  if (WiFi.status() == WL_CONNECTED) return "WiFi OK";
  if (WiFi.status() == WL_NO_SSID_AVAIL) return "WiFi?";
  if (s_wifi_started) return "WiFi...";
  return "WiFi --";
}

/* ------------------------------------------------------------------------- */
/*  Touch calibration (4 big squares + centre validation)                    */
/* ------------------------------------------------------------------------- */
static bool cal_wait_tap(uint16_t *rx, uint16_t *ry) {
  uint32_t start = millis();
  while (millis() - start < 90000) {
    if (s_tft.getTouchRawZ() > 300) {
      uint32_t sx = 0, sy = 0;
      int n = 0;
      while (s_tft.getTouchRawZ() > 300 && n < 10) {
        uint16_t a, b;
        s_tft.getTouchRaw(&a, &b);
        sx += a;
        sy += b;
        n++;
        delay(15);
      }
      if (n > 0) {
        *rx = (uint16_t)(sx / n);
        *ry = (uint16_t)(sy / n);
        return true;
      }
    }
    delay(15);
  }
  return false;
}

static void cal_draw_square(int i, uint16_t fill) {
  const int pos[4][2] = {{64, 76}, {LCD_WIDTH - 64, 76}, {64, LCD_HEIGHT - 76},
                         {LCD_WIDTH - 64, LCD_HEIGHT - 76}};
  int x = pos[i][0], y = pos[i][1];
  s_tft.fillRect(x - 44, y - 44, 88, 88, fill);
  s_tft.drawRect(x - 46, y - 46, 92, 92, TFT_WHITE);
  char num[4];
  snprintf(num, sizeof num, "%d", i + 1);
  s_tft.setTextColor(TFT_BLACK, fill);
  s_tft.drawString(num, x - 8, y - 16, 4);
}

static void run_touch_calibration(void) {
  for (int attempt = 0; attempt < 3; attempt++) {
    s_tft.fillScreen(TFT_BLACK);
    s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
    s_tft.drawString("KALIBRASI: ketuk kotak 1-4", 70, 6, 2);
    for (int i = 0; i < 4; i++) cal_draw_square(i, TFT_CYAN);
    delay(300);

    uint16_t raw[4][2];
    bool ok = true;
    for (int i = 0; i < 4 && ok; i++) {
      if (!cal_wait_tap(&raw[i][0], &raw[i][1])) {
        ok = false;
        break;
      }
      cal_draw_square(i, TFT_GREEN);
      delay(120);
    }
    if (!ok) {
      s_tft.drawString("BATAL (timeout)", 170, 160, 4);
      delay(900);
      ui_open_settings();
      return;
    }

    uint16_t v[8] = {raw[0][0], raw[0][1], raw[2][0], raw[2][1],
                     raw[1][0], raw[1][1], raw[3][0], raw[3][1]};
    bool rotate = abs((int)v[0] - (int)v[4]) > abs((int)v[1] - (int)v[5]);
    int x0, x1, y0, y1;
    if (rotate) {
      x0 = (v[1] + v[3]) / 2;
      x1 = (v[5] + v[7]) / 2;
      y0 = (v[0] + v[4]) / 2;
      y1 = (v[2] + v[6]) / 2;
    } else {
      x0 = (v[0] + v[2]) / 2;
      x1 = (v[4] + v[6]) / 2;
      y0 = (v[1] + v[5]) / 2;
      y1 = (v[3] + v[7]) / 2;
    }
    bool invx = x0 > x1, invy = y0 > y1;
    if (invx) { int t = x0; x0 = x1; x1 = t; }
    if (invy) { int t = y0; y0 = y1; y1 = t; }

    uint16_t cal[8];
    cal[0] = (uint16_t)x0;
    cal[1] = (uint16_t)(x1 - x0);
    cal[2] = (uint16_t)y0;
    cal[3] = (uint16_t)(y1 - y0);
    cal[4] = (uint16_t)((rotate ? 1 : 0) | (invx ? 2 : 0) | (invy ? 4 : 0));
    cal[5] = cal[6] = cal[7] = 0;
    s_tft.setTouch(cal);

    /* centre validation */
    s_tft.fillScreen(TFT_BLACK);
    s_tft.drawString("ketuk titik TENGAH", 140, 6, 2);
    s_tft.drawCircle(240, 160, 32, TFT_YELLOW);
    s_tft.drawFastVLine(240, 118, 84, TFT_YELLOW);
    s_tft.drawFastHLine(198, 160, 84, TFT_YELLOW);
    uint16_t cx = 0, cy = 0;
    bool got = false;
    uint32_t t0 = millis();
    while (millis() - t0 < 30000) {
      if (s_tft.getTouch(&cx, &cy, 300)) {
        got = true;
        break;
      }
      delay(15);
    }
    if (got && cx > 200 && cx < 280 && cy > 120 && cy < 200) {
      memcpy(g_settings.touch_cal, cal, sizeof cal);
      g_settings.touch_cal_valid = true;
      settings_save(&g_settings);
      Serial.printf("[touch] cal saved x0=%u x1=%u y0=%u y1=%u f=%u\n", cal[0], cal[1],
                    cal[2], cal[3], cal[4]);
      s_tft.fillScreen(TFT_BLACK);
      s_tft.setTextColor(TFT_GREEN, TFT_BLACK);
      s_tft.drawString("KALIBRASI SELESAI", 130, 140, 4);
      delay(1200);
      ui_open_settings();
      return;
    }
    if (g_settings.touch_cal_valid) s_tft.setTouch(g_settings.touch_cal);
    s_tft.setTextColor(TFT_RED, TFT_BLACK);
    s_tft.drawString("ULANGI KALIBRASI", 160, 280, 2);
    delay(1200);
  }
  s_tft.drawString("KALIBRASI GAGAL", 170, 140, 4);
  delay(1200);
  ui_open_settings();
}

/* ------------------------------------------------------------------------- */
/*  Serial console                                                           */
/* ------------------------------------------------------------------------- */
static char s_cmd_buf[160];
static size_t s_cmd_len = 0;

static void serial_help(void) {
  Serial.println("commands:");
  Serial.println("  status                     print settings + state");
  Serial.println("  wifi <ssid> <pass>         set Wi-Fi credentials");
  Serial.println("  mqtt <host> <port> <user> <pass>");
  Serial.println("  floor <label...>           title left part");
  Serial.println("  room <label...>            title right part");
  Serial.println("  bright <20-100>            LCD backlight percent (saved)");
  Serial.println("  theme [0-5]                list or set the colour theme");
  Serial.println("  lamps                      list every actuator and its label");
  Serial.println("  scope [0-2]                picker filter: 0 ruang, 1 lantai, 2 semua");
  Serial.println("  cat                        list floors + rooms from the controller");
  Serial.println("  devid <name>               MQTT device id (default hioto-genmon-xxxxxx)");
  Serial.println("  alias <n> <nama...>        friendly name for button n (1-8)");
  Serial.println("  alias <n> -                reset button n to the automatic name");
  Serial.println("  uigeom                     print keyboard / overlay geometry");
  Serial.println("  ver                        firmware version + OTA status");
  Serial.println("  ota                        check the OTA server now (may reboot)");
  Serial.println("  otaurl <url>               flash a firmware URL directly");
  Serial.println("  otaauto <0|1>              automatic OTA checks on/off");
  Serial.println("  id                         device GUID / MAC / registration state");
  Serial.println("  reg                        register with the controller now");
  Serial.println("  ldr                        raw LDR ADC");
  Serial.println("  cal | clearcal | traw      touch calibration");
  Serial.println("  save                       persist + reboot");
  Serial.println("  reboot");
}

static void serial_status(void) {
  Serial.printf("dev=%s floor=%s(%u) room=%s(%u)\n", g_settings.device_id,
                g_settings.floor_label, (unsigned)g_settings.floor_id,
                g_settings.room_label, (unsigned)g_settings.room_id);
  Serial.printf("guid=%s registration=%s\n", identity_guid(), identity_state());
  Serial.printf("wifi='%s' mqtt=%s:%u lampu=%d\n", g_settings.wifi_ssid,
                g_settings.mqtt_host, (unsigned)g_settings.mqtt_port, registry_count());
  Serial.printf("ip=%s ota=%s:3232\n", WiFi.localIP().toString().c_str(),
                s_ota_started ? "ready" : "off");
  Serial.printf("bright=%u%% theme=%u (%s) scope=%u\n", (unsigned)g_settings.brightness,
                (unsigned)g_settings.theme, theme_get(g_settings.theme)->name,
                (unsigned)g_settings.scope);
  for (int i = 0; i < GEN_SLOTS; i++) {
    int ri = registry_resolve(g_settings.slots[i].guid, g_settings.slots[i].label);
    Serial.printf("slot%d: %-16s '%s' %s\n", i + 1, g_settings.slots[i].label,
                  g_settings.slots[i].alias, ri >= 0
                                                ? (registry_state_known(ri)
                                                       ? (registry_state_on(ri) ? "ON" : "OFF")
                                                       : "?")
                                                : "(tidak ditemukan)");
  }
}

static void serial_handle_line(char *line) {
  char *tok[6];
  int n = 0;
  char *save = NULL;
  for (char *t = strtok_r(line, " \t\r", &save); t && n < 6; t = strtok_r(NULL, " \t\r", &save))
    tok[n++] = t;
  if (n == 0) return;

  if (!strcmp(tok[0], "help")) {
    serial_help();
  } else if (!strcmp(tok[0], "status")) {
    serial_status();
  } else if (!strcmp(tok[0], "wifi") && n >= 2) {
    snprintf(g_settings.wifi_ssid, sizeof g_settings.wifi_ssid, "%s", tok[1]);
    g_settings.wifi_pass[0] = '\0';
    if (n >= 3) snprintf(g_settings.wifi_pass, sizeof g_settings.wifi_pass, "%s", tok[2]);
    wifi_apply(g_settings.wifi_ssid, g_settings.wifi_pass);
    Serial.printf("[cfg] wifi ssid=%s\n", g_settings.wifi_ssid);
  } else if (!strcmp(tok[0], "mqtt") && n >= 3) {
    snprintf(g_settings.mqtt_host, sizeof g_settings.mqtt_host, "%s", tok[1]);
    g_settings.mqtt_port = (uint16_t)atoi(tok[2]);
    if (n >= 4) snprintf(g_settings.mqtt_user, sizeof g_settings.mqtt_user, "%s", tok[3]);
    g_settings.mqtt_pass[0] = '\0';
    if (n >= 5) snprintf(g_settings.mqtt_pass, sizeof g_settings.mqtt_pass, "%s", tok[4]);
    Serial.printf("[cfg] mqtt %s:%u\n", g_settings.mqtt_host, (unsigned)g_settings.mqtt_port);
  } else if (!strcmp(tok[0], "floor") && n >= 2) {
    char label[24] = "";
    for (int i = 1; i < n; i++) {
      if (i > 1) strncat(label, " ", sizeof label - strlen(label) - 1);
      strncat(label, tok[i], sizeof label - strlen(label) - 1);
    }
    snprintf(g_settings.floor_label, sizeof g_settings.floor_label, "%s", label);
    int fi = catalog_find_floor(0, g_settings.floor_label);
    g_settings.floor_id = (fi >= 0) ? catalog_floor_id(fi) : 0;
    ui_catalog_changed();
    Serial.printf("[cfg] floor=%s (id=%u)\n", g_settings.floor_label,
                  (unsigned)g_settings.floor_id);
  } else if (!strcmp(tok[0], "room") && n >= 2) {
    char label[24] = "";
    for (int i = 1; i < n; i++) {
      if (i > 1) strncat(label, " ", sizeof label - strlen(label) - 1);
      strncat(label, tok[i], sizeof label - strlen(label) - 1);
    }
    snprintf(g_settings.room_label, sizeof g_settings.room_label, "%s", label);
    int ri = catalog_find_room(g_settings.floor_id, 0, g_settings.room_label);
    g_settings.room_id = (ri >= 0) ? catalog_room_id(g_settings.floor_id, ri) : 0;
    ui_catalog_changed();
    Serial.printf("[cfg] room=%s (id=%u)\n", g_settings.room_label,
                  (unsigned)g_settings.room_id);
  } else if (!strcmp(tok[0], "uigeom")) {
    ui_log_geometry();
  } else if (!strcmp(tok[0], "id")) {
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    Serial.printf("guid=%s\nmac=%02x:%02x:%02x:%02x:%02x:%02x\ndevid=%s\nregistration=%s\n",
                  identity_guid(), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  g_settings.device_id, identity_state());
  } else if (!strcmp(tok[0], "reg")) {
    Serial.println("[id] mendaftar ke controller...");
    identity_register_now();
    Serial.printf("[id] %s\n", identity_state());
  } else if (!strcmp(tok[0], "ver")) {
    Serial.printf("fw=%s type=%s ota=%s auto=%u server=%s\n", ota_version(), FW_DEVICE_TYPE,
                  ota_status_text(), (unsigned)g_settings.ota_auto, OTA_BASE);
  } else if (!strcmp(tok[0], "ota")) {
    Serial.println("[ota] memeriksa server...");
    ota_check_now();
  } else if (!strcmp(tok[0], "otaurl") && n >= 2) {
    Serial.printf("[ota] flash %s\n", tok[1]);
    if (ota_flash_url(tok[1])) {
      Serial.println("[ota] selesai, reboot");
      delay(400);
      ESP.restart();
    }
  } else if (!strcmp(tok[0], "otaauto")) {
    if (n >= 2) {
      g_settings.ota_auto = (uint8_t)(atoi(tok[1]) ? 1 : 0);
      settings_save(&g_settings);
    }
    Serial.printf("[cfg] otaauto=%u\n", (unsigned)g_settings.ota_auto);
  } else if (!strcmp(tok[0], "lamps")) {    Serial.printf("[reg] %d aktuator (filter scope=%u)\n", registry_count(),
                  (unsigned)g_settings.scope);
    for (int i = 0; i < registry_count(); i++)
      Serial.printf("  %3d  %-16s %-20s %-26s%s\n", i + 1, registry_shortname(i),
                    registry_floor_at(i), registry_room_at(i),
                    registry_state_known(i) ? (registry_state_on(i) ? " ON" : " OFF")
                                            : "");
  } else if (!strcmp(tok[0], "alias") && n >= 3) {
    const int idx = atoi(tok[1]) - 1;
    if (idx < 0 || idx >= GEN_SLOTS) {
      Serial.println("[cfg] alias: nomor tombol 1-8");
    } else if (!strcmp(tok[2], "-")) {
      g_settings.slots[idx].alias[0] = '\0';
      settings_save(&g_settings);
      Serial.printf("[cfg] slot%d alias otomatis\n", idx + 1);
    } else {
      char text[24] = "";
      for (int i = 2; i < n; i++) {
        if (i > 2) strncat(text, " ", sizeof text - strlen(text) - 1);
        strncat(text, tok[i], sizeof text - strlen(text) - 1);
      }
      snprintf(g_settings.slots[idx].alias, sizeof g_settings.slots[idx].alias, "%s", text);
      settings_save(&g_settings);
      Serial.printf("[cfg] slot%d alias='%s'\n", idx + 1, g_settings.slots[idx].alias);
    }
  } else if (!strcmp(tok[0], "cat")) {
    Serial.printf("[cat] %d lantai:\n", catalog_floor_count());
    for (int i = 0; i < catalog_floor_count(); i++)
      Serial.printf("  floor id=%u  %s\n", (unsigned)catalog_floor_id(i),
                    catalog_floor_name(i));
    Serial.printf("[cat] %d ruang:\n", catalog_room_count(0));
    if (catalog_floor_count() > 0) {
      for (int f = 0; f < catalog_floor_count(); f++) {
        const uint8_t fid = catalog_floor_id(f);
        const int n = catalog_room_count(fid);
        for (int r = 0; r < n; r++)
          Serial.printf("  floor %u  room id=%u  %s\n", (unsigned)fid,
                        (unsigned)catalog_room_id(fid, r), catalog_room_name(fid, r));
      }
    }
  } else if (!strcmp(tok[0], "devid") && n >= 2) {
    snprintf(g_settings.device_id, sizeof g_settings.device_id, "%s", tok[1]);
    settings_save(&g_settings);
    Serial.printf("[cfg] devid=%s (reboot to apply)\n", g_settings.device_id);
  } else if (!strcmp(tok[0], "scope")) {
    if (n >= 2) {
      int sc = atoi(tok[1]);
      if (sc < 0 || sc > 2) sc = 0;
      g_settings.scope = (uint8_t)sc;
      settings_save(&g_settings);
      ui_registry_changed();
      Serial.printf("[cfg] scope=%d\n", sc);
    } else {
      Serial.printf("[cfg] scope=%u (0=ruang 1=lantai 2=semua)\n",
                    (unsigned)g_settings.scope);
    }
  } else if (!strcmp(tok[0], "bright")) {
    if (n >= 2) {
      int pct = atoi(tok[1]);
      if (pct < 20) pct = 20;
      if (pct > 100) pct = 100;
      g_settings.brightness = (uint8_t)pct;
      backlight_set(g_settings.brightness);
      settings_save(&g_settings);
      Serial.printf("[cfg] bright=%d%%\n", pct);
    } else {
      Serial.printf("[cfg] bright=%u%%\n", (unsigned)backlight_get());
    }
  } else if (!strcmp(tok[0], "theme")) {
    if (n >= 2) {
      int idx = atoi(tok[1]);
      if (idx < 0 || idx >= THEME_COUNT) idx = 0;
      g_settings.theme = (uint8_t)idx;
      settings_save(&g_settings);
      ui_theme_changed();
      Serial.printf("[cfg] theme=%d (%s)\n", idx, theme_get(g_settings.theme)->name);
    }
    for (int i = 0; i < THEME_COUNT; i++)
      Serial.printf("  %d = %s\n", i, g_themes[i].name);
  } else if (!strcmp(tok[0], "ldr")) {
    int adc = analogRead(PIN_LDR);
    int pct = (adc - 300) * 100 / (3900 - 300);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    Serial.printf("[ldr] adc=%d pct=%d%%\n", adc, pct);
  } else if (!strcmp(tok[0], "cal")) {
    run_touch_calibration();
  } else if (!strcmp(tok[0], "clearcal")) {
    Preferences prefs;
    if (prefs.begin("hioto", false)) {
      prefs.remove("tcal");
      prefs.end();
    }
    g_settings.touch_cal_valid = false;
    Serial.println("[cfg] calibration cleared; rebooting");
    delay(300);
    ESP.restart();
  } else if (!strcmp(tok[0], "save")) {
    settings_save(&g_settings);
    Serial.println("[cfg] saved; rebooting...");
    delay(300);
    ESP.restart();
  } else if (!strcmp(tok[0], "reboot")) {
    Serial.println("[cfg] rebooting...");
    delay(200);
    ESP.restart();
  } else {
    Serial.printf("[cfg] unknown '%s' (try help)\n", tok[0]);
  }
}

static void serial_poll(void) {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (s_cmd_len > 0) {
        s_cmd_buf[s_cmd_len] = '\0';
        serial_handle_line(s_cmd_buf);
        s_cmd_len = 0;
      }
    } else if (s_cmd_len < sizeof s_cmd_buf - 1) {
      s_cmd_buf[s_cmd_len++] = c;
    }
  }
}

/* ------------------------------------------------------------------------- */
void setup() {
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  digitalWrite(PIN_LED_R, HIGH);
  digitalWrite(PIN_LED_G, HIGH);
  digitalWrite(PIN_LED_B, HIGH);

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== HIOTO General Monitor / switch panel / v1.0 ===");
  Serial.printf("[boot] chip=%s flash=%uKB psram=%uKB\n", ESP.getChipModel(),
                (unsigned)(ESP.getFlashChipSize() / 1024),
                (unsigned)(ESP.getPsramSize() / 1024));

  lv_init();
  s_tft.init();
  s_tft.setRotation(1);
  s_tft.fillScreen(TFT_BLACK);

  settings_load(&g_settings);
  backlight_begin(g_settings.brightness);
  Serial.printf("[cfg] dev=%s floor=%s room=%s mqtt=%s:%u\n", g_settings.device_id,
                g_settings.floor_label, g_settings.room_label, g_settings.mqtt_host,
                g_settings.mqtt_port);
  Serial.printf("[cfg] bright=%u%% theme=%s\n", (unsigned)g_settings.brightness,
                theme_get(g_settings.theme)->name);

  const uint32_t px = LCD_WIDTH * kDrawLines;
  s_buf_a = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s_buf_b = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_buf_a || !s_buf_b) {
    Serial.println("[boot] FATAL: no RAM for LVGL buffers");
    while (true) delay(1000);
  }

  lv_disp_draw_buf_init(&s_draw_buf, s_buf_a, s_buf_b, px);
  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = LCD_WIDTH;
  s_disp_drv.ver_res = LCD_HEIGHT;
  s_disp_drv.flush_cb = lv_flush_cb;
  s_disp_drv.draw_buf = &s_draw_buf;
  lv_disp_drv_register(&s_disp_drv);

  if (g_settings.touch_cal_valid) {
    s_tft.setTouch(g_settings.touch_cal);
    Serial.println("[touch] saved calibration applied");
  }
  static lv_indev_drv_t s_indev_drv;
  lv_indev_drv_init(&s_indev_drv);
  s_indev_drv.type = LV_INDEV_TYPE_POINTER;
  s_indev_drv.read_cb = indev_read_cb;
  lv_indev_drv_register(&s_indev_drv);

  registry_begin();
  catalog_begin();
  ota_begin();
  ui_init();
  wifi_apply(g_settings.wifi_ssid, g_settings.wifi_pass);
  identity_begin();
  mqtt_begin(&g_settings);

  pinMode(PIN_BOOT, INPUT_PULLUP);

  if (!g_settings.wifi_ssid[0]) {
    Serial.println("[boot] no Wi-Fi credentials -> opening settings");
    ui_open_settings();
  }
  Serial.println("[ui] ready");
}

void loop() {
  lv_timer_handler();
  ui_poll();
  mqtt_poll();

  if (wifi_ok()) {
    if (!s_ota_started) ard_ota_begin();
    ArduinoOTA.handle();
  }

  if (registry_poll()) {
    ui_registry_changed();
    ui_refresh_states();
  }
  if (catalog_poll()) ui_catalog_changed();
  identity_poll();
  ota_poll();

  if (millis() - s_last_ui > 250) {
    s_last_ui = millis();
    ui_refresh_states();
  }
  serial_poll();

  /* BOOT: short press opens settings; hold >= 5 s = factory reset.
   * The button is ignored until it has been seen released once, so holding
   * BOOT to enter download mode can never wipe the settings on the next boot. */
  const bool btn = (digitalRead(PIN_BOOT) == LOW);
  if (!btn) s_btn_armed = true;
  if (s_btn_armed) {
    if (btn && !s_btn_prev) {
      s_btn_pressed_at = millis();
      s_btn_long_done = false;
      ui_open_settings();
      delay(30);
    } else if (btn && !s_btn_long_done && millis() - s_btn_pressed_at > 5000) {
      s_btn_long_done = true;
      Serial.println("[boot] factory reset (hold 5 s)");
      settings_factory_reset();
      delay(800);
      ESP.restart();
    }
  }
  s_btn_prev = btn;

  delay(5);
}
