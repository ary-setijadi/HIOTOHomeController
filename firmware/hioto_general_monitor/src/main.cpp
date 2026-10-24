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
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "board_pins.h"
#include "touch.h"
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

/* Full touch hardware diagnostic ("tpdiag"); detailed on the 2.8" board,
 * a short notice elsewhere. */
void touch_diag_full(void);

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
/*  Display self test: cycles all four rotations with corner markers so the   */
/*  correct orientation / geometry / colour order can be identified by eye.  */
/* ------------------------------------------------------------------------- */
static void draw_disp_test(int rot) {
  s_tft.setRotation((uint8_t)rot);
  const int w = (int)s_tft.width();
  const int h = (int)s_tft.height();
  s_tft.fillScreen(TFT_BLACK);
  s_tft.drawRect(0, 0, w, h, TFT_WHITE);
  s_tft.fillRect(0, 0, 34, 34, TFT_RED);              /* kiri-atas */
  s_tft.fillRect(w - 34, 0, 34, 34, TFT_GREEN);       /* kanan-atas */
  s_tft.fillRect(0, h - 34, 34, 34, TFT_BLUE);        /* kiri-bawah */
  s_tft.fillRect(w - 34, h - 34, 34, 34, TFT_YELLOW); /* kanan-bawah */
  s_tft.drawFastHLine(0, h / 2, w, TFT_CYAN);
  s_tft.drawFastVLine(w / 2, 0, h, TFT_CYAN);
  s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
  s_tft.setCursor(6, h / 2 - 34);
  s_tft.setTextFont(4);
  s_tft.printf("ROT %d", rot);
  s_tft.setCursor(6, h / 2 - 8);
  s_tft.setTextFont(2);
  s_tft.printf("%dx%d", w, h);
  s_tft.setCursor(6, h / 2 + 10);
  s_tft.setTextFont(1);
  s_tft.printf("merah=kiri-atas hijau=kanan-atas");
  s_tft.setCursor(6, h / 2 + 20);
  s_tft.printf("biru=kiri-bawah kuning=kanan-bawah");
  s_tft.setCursor(6, h - 20);
  s_tft.printf("sentuh untuk berhenti");
  Serial.printf("[disp] test rot=%d panel=%dx%d\n", rot, w, h);
}

void display_self_test(void) {
  bool stop = false;
  for (int r = 0; r < 4 && !stop; r++) {
    draw_disp_test(r);
    const uint32_t t0 = millis();
    while (millis() - t0 < 6000) {
      if (touch_is_down()) {
        stop = true;
        break;
      }
      if (Serial.available()) {
        while (Serial.available()) Serial.read();
        stop = true;
        break;
      }
      delay(20);
    }
  }
  s_tft.setRotation(g_settings.rot);
  Serial.printf("[disp] selesai; rot=%u\n", (unsigned)g_settings.rot);
  ui_open_settings();
}

/* ------------------------------------------------------------------------- */
/*  Performance counters                                                     */
/*                                                                           */
/*  "sluggish" needs numbers: a slow panel usually means either the touch read */
/*  is expensive (the XPT2046 is bit-banged by hand on the 2.8") or the flush */
/*  is pushing more pixels than the SPI bus can carry. Counters are folded in */
/*  loop() and reported by the "perf" console command.                       */
/* ------------------------------------------------------------------------- */
struct PerfStats {
  uint32_t loops, loop_ms;
  uint32_t touch_calls, touch_us, touch_max_us;
  uint32_t flush_calls, flush_us, flush_px;
  uint32_t lv_us, lv_max_us;
  uint32_t gap_max_us;
};
static PerfStats s_perf;       /* current window, reset every 2 s */
static PerfStats s_perf_last;  /* last completed window */
static uint32_t s_perf_t0 = 0;
static uint32_t s_loop_last_us = 0;

static inline void perf_window(void) {
  const uint32_t now = millis();
  if (!s_perf_t0) {
    s_perf_t0 = now;
    return;
  }
  if (now - s_perf_t0 < 2000) return;
  s_perf.loop_ms = now - s_perf_t0;
  s_perf_last = s_perf;
  memset(&s_perf, 0, sizeof s_perf);
  s_perf_t0 = now;
}

static void perf_print(void) {
  const PerfStats *p = &s_perf_last;
  const uint32_t ms = p->loop_ms ? p->loop_ms : 1;
  Serial.printf("[perf] window=%ums loops/s=%u lv_avg=%uus lv_max=%uus\n", ms,
                (unsigned)(p->loops * 1000 / ms),
                (unsigned)(p->lv_us / (p->loops ? p->loops : 1)), (unsigned)p->lv_max_us);
  /* gap = worst time the loop was away. A large value with a small lv_max means
   * something OUTSIDE lv_timer_handler blocked - typically an HTTP fetch or an
   * MQTT connect with the broker unreachable. That is what makes the panel feel
   * sluggish while the UI code itself is idle. */
  Serial.printf("[perf] gap_avg=%ums gap_max=%ums (blocked outside LVGL)\n",
                (unsigned)(ms / (p->loops ? p->loops : 1)), (unsigned)(p->gap_max_us / 1000));
  Serial.printf("[perf] touch/s=%u touch_avg=%uus touch_max=%uus\n",
                (unsigned)(p->touch_calls * 1000 / ms),
                (unsigned)(p->touch_us / (p->touch_calls ? p->touch_calls : 1)),
                (unsigned)p->touch_max_us);
  Serial.printf("[perf] flush/s=%u flush_avg=%uus px/s=%u (~%u KB/s at 16bpp)\n",
                (unsigned)(p->flush_calls * 1000 / ms),
                (unsigned)(p->flush_us / (p->flush_calls ? p->flush_calls : 1)),
                (unsigned)(p->flush_px * 1000 / ms), (unsigned)(p->flush_px * 2000 / ms / 1024));
  Serial.printf("[perf] heap=%u largest=%u min_free=%u\n", (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getMinFreeHeap());
}



/* ------------------------------------------------------------------------- */
/*  LVGL flush                                                               */
/* ------------------------------------------------------------------------- */
static void lv_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  const uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
  const uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
  const uint32_t t0 = micros();
  s_tft.startWrite();
  s_tft.setAddrWindow(area->x1, area->y1, w, h);
  s_tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  s_tft.endWrite();
  const uint32_t dt = micros() - t0;
  s_perf.flush_calls++;
  s_perf.flush_us += dt;
  s_perf.flush_px += w * h;
  lv_disp_flush_ready(disp);
}

/* ------------------------------------------------------------------------- */
/*  Touch                                                                    */
/* ------------------------------------------------------------------------- */
static void indev_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  (void)drv;
  int x = 0, y = 0;
  const uint32_t t0 = micros();
  const bool down = touch_point(&x, &y);
  const uint32_t dt = micros() - t0;
  s_perf.touch_calls++;
  s_perf.touch_us += dt;
  if (dt > s_perf.touch_max_us) s_perf.touch_max_us = dt;
  if (down) {
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
/*  Touch calibration (delegates to touch.cpp, which knows the topology)     */
/* ------------------------------------------------------------------------- */
void touch_calibrate(void) { touch_calibrate_ui(); }
bool touch_check_target(int tx, int ty, int *err_x, int *err_y);

/* ------------------------------------------------------------------------- */
/*  Wrong-board guard                                                        */
/*                                                                           */
/*  A CYD that is flashed with the other panel's firmware looks completely    */
/*  dead - the init sequence belongs to a different controller - and there is */
/*  no runtime hint about it. RDDID (0x04) answers 0x7796 on an ST7796 and    */
/*  0x7789 on an ST7789, so a mismatch against the compiled driver can be     */
/*  reported loudly instead. Boards whose MISO is not wired simply return     */
/*  nonsense, in which case nothing is claimed.                              */
/* ------------------------------------------------------------------------- */
#if defined(ST7796_DRIVER)
#define DISP_EXPECTED_ID 0x7796
#define DISP_EXPECTED_NAME "ST7796"
#elif defined(ST7789_DRIVER)
#define DISP_EXPECTED_ID 0x7789
#define DISP_EXPECTED_NAME "ST7789"
#elif defined(ILI9341_DRIVER) || defined(ILI9341_2_DRIVER)
#define DISP_EXPECTED_ID 0x9341
#define DISP_EXPECTED_NAME "ILI9341"
#else
#define DISP_EXPECTED_ID 0
#define DISP_EXPECTED_NAME "unknown"
#endif

static void display_id_report(void) {
  uint8_t b[5] = {0, 0, 0, 0, 0};
  for (int i = 0; i < 5; i++) b[i] = s_tft.readcommand8(0x04, (uint8_t)i);

  /* The three ID bytes can land at different indices depending on the driver's
   * dummy-bit handling, so scan for the pair instead of assuming an offset. */
  int found = 0;
  for (int i = 0; i + 1 < 5; i++) {
    const int id = ((int)b[i] << 8) | b[i + 1];
    if (id == 0x7796 || id == 0x7789 || id == 0x9341) {
      found = id;
      break;
    }
  }

  Serial.printf("[disp] id=%02X %02X %02X %02X %02X harap=%s(0x%04X)\n", b[0], b[1], b[2],
                b[3], b[4], DISP_EXPECTED_NAME, (unsigned)DISP_EXPECTED_ID);

  if (!found) {
    Serial.println("[disp] id tidak terbaca (MISO?) - tidak bisa dipastikan");
  } else if (DISP_EXPECTED_ID && found != DISP_EXPECTED_ID) {
    Serial.println("[disp] !!! SALAH FIRMWARE UNTUK BOARD INI !!!");
    Serial.printf("[disp] panel menjawab 0x%04X tapi driver terkompilasi %s.\n",
                  (unsigned)found, DISP_EXPECTED_NAME);
    Serial.println("[disp] Layar akan tampak mati. Flash firmware tipe device yang benar.");
  } else {
    Serial.println("[disp] cocok");
  }
}

/* Wrong-board detector. RDDID above depends on the panel wiring its SDO line
 * and on TFT_eSPI supporting the read, which ST7796/ST7789 do not (those reads
 * come back 0xFF), so the reliable signal comes from NVS: the first firmware
 * that ever boots on a board stamps itself here. A later boot that disagrees is
 * almost certainly a wrong flash - exactly the failure that makes a perfectly
 * healthy CYD look dead.
 *
 * The comparison is on the BOARD token ("35" / "28"), not the full type string:
 * a channel rename (adding the chipset, say) must not cry wolf, while a 28<->35
 * mix-up - the one that actually blanks a screen - always does. */
static int board_token(const char *type) {
  const char *g = strstr(type, "genmon");
  if (!g) return -1;
  g += 6;
  while (*g == '-' || *g == '_') g++;
  return atoi(g); /* 0 when the token is missing or not numeric */
}

static void board_tag_check(void) {
  Preferences p;
  if (!p.begin("hioto", false)) return;
  const String tag = p.getString("btag", "");
  if (tag.length() == 0) {
    p.putString("btag", FW_DEVICE_TYPE);
    p.end();
    Serial.printf("[board] tag disimpan: %s (%s)\n", FW_DEVICE_TYPE, FW_PANEL_CHIP);
    return;
  }
  const int want = board_token(FW_DEVICE_TYPE);
  const int got = board_token(tag.c_str());
  if (want && got && want != got) {
    Serial.println("[board] !!! FIRMWARE TIDAK COCOK DENGAN BOARD INI !!!");
    Serial.printf("[board] board ini terakhir menjalankan '%s', sekarang '%s'\n", tag.c_str(),
                  FW_DEVICE_TYPE);
    Serial.println("[board] Layar bisa tampak mati padahal board sehat.");
    Serial.println("[board] Flash firmware dengan tipe device yang benar untuk board ini.");
  } else {
    if (tag != FW_DEVICE_TYPE) {
      /* same board size, new channel name: adopt it silently */
      p.putString("btag", FW_DEVICE_TYPE);
      Serial.printf("[board] tag diperbarui: %s -> %s\n", tag.c_str(), FW_DEVICE_TYPE);
    } else {
      Serial.printf("[board] tag cocok: %s\n", FW_DEVICE_TYPE);
    }
  }
  p.end();
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
  Serial.println("  disp [0-3]                 TFT rotation (saved, reboots)");
  Serial.println("  cal                        touch calibration");
  Serial.println("  slot <n> <LAMP.xx>         assign button n (1-8) to a device");
  Serial.println("  dispid                     read the display controller id (wrong-board check)");
  Serial.println("  perf                       loop / touch / flush timings + heap");
  Serial.println("  disptest                   cycle all 4 rotations with corner markers");
  Serial.println("  parts                      OTA slot layout + image versions");
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
  Serial.println("  dl                         reboot into the ROM serial bootloader (no BOOT button)");
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
  } else if (!strcmp(tok[0], "disptest")) {
    display_self_test();
  } else if (!strcmp(tok[0], "disp")) {
    if (n >= 2) {
      int r = atoi(tok[1]);
      if (r < 0 || r > 3) r = 0;
      g_settings.rot = (uint8_t)r;
      settings_save(&g_settings);
      Serial.printf("[cfg] rot=%d; rebooting\n", r);
      delay(300);
      ESP.restart();
    } else {
      Serial.printf("[cfg] rot=%u panel=%dx%d (0/2 = portrait, 1/3 = landscape)\n",
                    (unsigned)g_settings.rot, (int)s_tft.width(), (int)s_tft.height());
    }
  } else if (!strcmp(tok[0], "parts")) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    Serial.printf("[part] run=%s boot=%s fw=%s\n", run ? run->label : "?",
                  boot ? boot->label : "?", FW_VERSION);
    for (int i = 0; i < 2; i++) {
      const esp_partition_t *p = esp_partition_find_first(
          ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + i),
          NULL);
      if (!p) {
        Serial.printf("[part] ota%d: tidak ada\n", i);
        continue;
      }
      uint32_t magic = 0;
      esp_partition_read(p, 0, &magic, sizeof magic);
      esp_app_desc_t desc;
      const bool has = esp_ota_get_partition_description(p, &desc) == ESP_OK;
      Serial.printf("[part] %s @0x%06x size=0x%06x magic=0x%08x ver=%s\n", p->label,
                    (unsigned)p->address, (unsigned)p->size, (unsigned)magic,
                    has ? desc.version : "-");
    }
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
    Serial.printf("fw=%s type=%s chip=%s ota=%s auto=%u server=%s\n", ota_version(),
                FW_DEVICE_TYPE, FW_PANEL_CHIP,
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
  } else if (!strcmp(tok[0], "tptarget")) {
    /* Accuracy check with a drawn target, so the number means something. */
    const int tw = (int)s_tft.width(), th = (int)s_tft.height();
    int ex = 0, ey = 0;
    Serial.printf("[tp] target tengah (%d,%d) - ketuk lingkaran putih\n", tw / 2, th / 2);
    if (touch_check_target(tw / 2, th / 2, &ex, &ey)) {
      Serial.printf("[tp] meleset dx=%d dy=%d px (batas wajar +/-20)\n", ex, ey);
    } else {
      Serial.println("[tp] tidak ada ketukan");
    }
    ui_after_calibration();
  } else if (!strcmp(tok[0], "perf")) {
    perf_print();
  } else if (!strcmp(tok[0], "dispid")) {
    display_id_report();
  } else if (!strcmp(tok[0], "slot")) {
    const int idx = atoi(tok[1]) - 1;
    if (idx < 0 || idx >= GEN_SLOTS || !tok[2]) {
      Serial.println("[cfg] pakai: slot <1..8> <LAMP.xx>   (atau 'lamps' untuk daftar)");
      for (int i = 0; i < GEN_SLOTS; i++) {
        const int ri = registry_resolve(g_settings.slots[i].guid, g_settings.slots[i].label);
        Serial.printf("[cfg] slot%d = %-16s %s\n", i + 1, g_settings.slots[i].label,
                      ri >= 0 ? "ok" : "TIDAK DITEMUKAN");
      }
    } else {
      snprintf(g_settings.slots[idx].label, sizeof g_settings.slots[idx].label, "%s", tok[2]);
      g_settings.slots[idx].guid[0] = 0; /* resolve by label */
      const int ri = registry_resolve("", g_settings.slots[idx].label);
      if (ri >= 0)
        snprintf(g_settings.slots[idx].guid, sizeof g_settings.slots[idx].guid, "%s",
                 registry_guid_at(ri));
      settings_save(&g_settings);
      Serial.printf("[cfg] slot%d = %s -> %s\n", idx + 1, g_settings.slots[idx].label,
                    ri >= 0 ? registry_fullname(ri) : "TIDAK DITEMUKAN");
      ui_refresh_states(); /* repaint names/state; a full rebuild from the console
                            * is not needed and collided with ui_poll() before */
    }
  } else if (!strcmp(tok[0], "tp")) {
    Serial.println("[tp] sampling 20s - TEKAN & TAHAN layar sekarang");
    for (int i = 0; i < 200; i++) {
      const uint16_t z0 = touch_debug_variant(0xB0, 0);
      const uint16_t x0 = touch_debug_variant(0xD0, 0);
      const uint16_t y0 = touch_debug_variant(0x90, 0);
      const uint16_t x1 = touch_debug_variant(0xD0, 1);
      const uint16_t y1 = touch_debug_variant(0x90, 1);
      Serial.printf("[tp] %3d irq=%d z=%4u | rise x=%4u y=%4u | fall x=%4u y=%4u\n", i,
                    (int)touch_is_down(), (unsigned)z0, (unsigned)x0, (unsigned)y0,
                    (unsigned)x1, (unsigned)y1);
      delay(100);
    }
    Serial.println("[tp] selesai");
  } else if (!strcmp(tok[0], "tpprobe")) {
    Serial.println("[tp] port probe: 8 pembacaan (tekan layar)");
    uint16_t x = 0, y = 0, z = 0;
    touch_probe_read(&x, &y, &z);
    Serial.printf("[tp] probe: irq=%d x=%u y=%u z=%u\n", (int)touch_is_down(),
                  (unsigned)x, (unsigned)y, (unsigned)z);
  } else if (!strcmp(tok[0], "tptest")) {
    /* Verification after calibration: reports the SCREEN coordinate that the
     * production path (touch_point -> ui) produces, so a tap on a known spot
     * can be checked against where the firmware thinks it landed. */
    Serial.println("[tp] uji koordinat layar 20s - ketuk TENGAH layar");
    const uint32_t t0 = millis();
    bool was_down = false;
    while (millis() - t0 < 20000) {
      int x = 0, y = 0;
      const bool down = touch_point(&x, &y);
      if (down && !was_down) Serial.printf("[tp] TEKAN -> layar x=%d y=%d\n", x, y);
      was_down = down;
      delay(25);
    }
    Serial.println("[tp] selesai");
  } else if (!strcmp(tok[0], "tpdiag")) {
    touch_diag_full();
  } else if (!strcmp(tok[0], "cal")) {    touch_calibrate();
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
  settings_load(&g_settings);
  s_tft.setRotation(g_settings.rot);
  s_tft.fillScreen(TFT_BLACK);

  /* Ask the panel for its real size after rotation: a CYD whose glass is
   * mounted the other way reports a portrait area, and the UI must follow. */
  const lv_coord_t scr_w = (lv_coord_t)s_tft.width();
  const lv_coord_t scr_h = (lv_coord_t)s_tft.height();
  Serial.printf("[boot] rot=%u panel=%dx%d\n", (unsigned)g_settings.rot, (int)scr_w,
                (int)scr_h);
  display_id_report();
  board_tag_check();

  backlight_begin(g_settings.brightness);
  Serial.printf("[cfg] dev=%s floor=%s room=%s mqtt=%s:%u\n", g_settings.device_id,
                g_settings.floor_label, g_settings.room_label, g_settings.mqtt_host,
                g_settings.mqtt_port);
  Serial.printf("[cfg] bright=%u%% theme=%s\n", (unsigned)g_settings.brightness,
                theme_get(g_settings.theme)->name);

  const uint32_t px = (uint32_t)scr_w * kDrawLines;
  s_buf_a = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s_buf_b = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_buf_a || !s_buf_b) {
    Serial.println("[boot] FATAL: no RAM for LVGL buffers");
    while (true) delay(1000);
  }

  lv_disp_draw_buf_init(&s_draw_buf, s_buf_a, s_buf_b, px);
  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = scr_w;
  s_disp_drv.ver_res = scr_h;
  s_disp_drv.flush_cb = lv_flush_cb;
  s_disp_drv.draw_buf = &s_draw_buf;
  lv_disp_drv_register(&s_disp_drv);

  static lv_indev_drv_t s_indev_drv;
  lv_indev_drv_init(&s_indev_drv);
  s_indev_drv.type = LV_INDEV_TYPE_POINTER;
  s_indev_drv.read_cb = indev_read_cb;
  lv_indev_drv_register(&s_indev_drv);

  registry_begin();
  catalog_begin();
  ota_begin();
  ui_set_screen(scr_w, scr_h);
  touch_begin(&s_tft);
  touch_set_screen(scr_w, scr_h);
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
  /* worst gap between loop iterations: reveals blocking work done outside LVGL */
  {
    const uint32_t now_us = micros();
    if (s_loop_last_us) {
      const uint32_t gap = now_us - s_loop_last_us;
      if (gap > s_perf.gap_max_us) s_perf.gap_max_us = gap;
    }
    s_loop_last_us = now_us;
  }
  {
    const uint32_t t0 = micros();
    lv_timer_handler();
    const uint32_t dt = micros() - t0;
    s_perf.loops++;
    s_perf.lv_us += dt;
    if (dt > s_perf.lv_max_us) s_perf.lv_max_us = dt;
    perf_window();
  }
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
  delay(2); /* was 5: LVGL is serviced from this loop, so keep input latency low */
}
