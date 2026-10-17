/**
 * HIOTO Floor Monitor — Type F · v1.0
 * ---------------------------------------------------------------------------
 *  Board: Sunton ESP32-3248S035 (classic 3.5" CYD)
 *    - ESP32-D0WD-V3 (dual core 240 MHz), 4 MB flash, no PSRAM
 *    - ST7796 480x320 SPI display (HSPI pins), BL on GPIO27
 *    - XPT2046 resistive touch on the shared SPI bus (CS=GPIO33)
 *    - LDR on GPIO34, RGB status LED on 4/16/17, BOOT on GPIO0
 *    - CH340 USB-UART (COM3) — serial console @115200
 *
 *  A HIOTO "compound floor" monitor: subscribes to the broker, shows the
 *  live state of every device it can see on the floor (iot/... topics and the
 *  legacy HIOTO Sensor/Status/sensor topics and Log), publishes its own
 *  telemetry on iot/{id}/telemetry and status on iot/{id}/status.
 *
 *  Boot flow: NVS settings -> Wi-Fi (or captive portal) -> MQTT -> LVGL UI.
 *  BOOT short press cycles pages; hold >= 4 s clears NVS (factory reset).
 *
 *  NOTE: this board needs manual download mode to flash — hold BOOT, tap
 *  RESET, then flash with --before no_reset (see README.md).
 */
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <Preferences.h>

#include "board_pins.h"
#include "config.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "mqtt_mgr.h"
#include "monitor.h"
#include "registry.h"
#include "ui.h"

static TFT_eSPI s_tft;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;

static const uint32_t kDrawLines = 20; /* lines per LVGL draw buffer */
static lv_color_t *s_buf_a = nullptr;
static lv_color_t *s_buf_b = nullptr;

static uint8_t s_backlight_pct = 90;
static bool s_btn_prev = false;
static uint32_t s_btn_pressed_at = 0;
static bool s_btn_long_done = false;
static uint32_t s_last_ui = 0;
static uint32_t s_last_led = 0;

/* ------------------------------------------------------------------------- */
/*  Backlight — PWM on GPIO27 (active HIGH).                                 */
/* ------------------------------------------------------------------------- */
void ui_backlight_set(int pct) {
  static bool s_ledc_ready = false;
  if (pct < 5) pct = 5;
  if (pct > 100) pct = 100;
  s_backlight_pct = (uint8_t)pct;
  if (!s_ledc_ready) {
    ledcSetup(0, 25000, 10); /* 25 kHz, 10-bit */
    ledcAttachPin(PIN_LCD_BL, 0);
    s_ledc_ready = true;
  }
  ledcWrite(0, (uint32_t)s_backlight_pct * 1023U / 100U);
}

/* ------------------------------------------------------------------------- */
/*  LVGL -> TFT_eSPI flush                                                   */
/* ------------------------------------------------------------------------- */
static void lv_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  const uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
  const uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
  s_tft.startWrite();
  s_tft.setAddrWindow(area->x1, area->y1, w, h);
  s_tft.pushColors((uint16_t *)&color_p->full, w * h, true); /* swap for ST7796 */
  s_tft.endWrite();
  lv_disp_flush_ready(disp);
}

/* ------------------------------------------------------------------------- */
/*  Touch — XPT2046 via TFT_eSPI's built-in touch extension (TOUCH_CS=33).   */
/* ------------------------------------------------------------------------- */
static void indev_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  (void)drv;
  uint16_t x = 0, y = 0;
  if (s_tft.getTouch(&x, &y, 400)) {
    data->point.x = (lv_coord_t)x;
    data->point.y = (lv_coord_t)y;
    data->state = LV_INDEV_STATE_PRESSED;
    static uint32_t last = 0;
    if (millis() - last > 400) {
      last = millis();
      Serial.printf("[touch] x=%u y=%u\n", (unsigned)x, (unsigned)y);
    }
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

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

static int s_pos_cal[4][2];

static void draw_cal_square(int i, uint16_t fill) {
  int x = s_pos_cal[i][0], y = s_pos_cal[i][1];
  s_tft.fillRect(x - 44, y - 44, 88, 88, fill);
  s_tft.drawRect(x - 46, y - 46, 92, 92, TFT_WHITE);
  char num[4];
  snprintf(num, sizeof num, "%d", i + 1);
  s_tft.setTextColor(TFT_BLACK, fill);
  s_tft.drawString(num, x - 8, y - 16, 4);
}

/* Big numbered squares (number inside), tap 1-4, then a center-point
 * validation before the calibration is accepted and stored. */
static void run_touch_calibration(void) {
  for (int attempt = 0; attempt < 3; attempt++) {
    Serial.printf("[touch] calibration attempt %d\n", attempt + 1);
    s_tft.fillScreen(TFT_BLACK);
    s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
    s_tft.drawString("KALIBRASI: ketuk kotak 1-4", 80, 6, 2);

    const int ix = 64, iy = 76;
    const int pos[4][2] = {{ix, iy}, {LCD_WIDTH - ix, iy}, {ix, LCD_HEIGHT - iy},
                           {LCD_WIDTH - ix, LCD_HEIGHT - iy}};
    memcpy(s_pos_cal, pos, sizeof s_pos_cal);
    for (int i = 0; i < 4; i++) draw_cal_square(i, TFT_CYAN);
    delay(300);

    uint16_t raw[4][2];
    bool ok = true;
    for (int i = 0; i < 4 && ok; i++) {
      if (!cal_wait_tap(&raw[i][0], &raw[i][1])) {
        ok = false;
        break;
      }
      Serial.printf("[touch] tap %d: raw %u,%u\n", i + 1, (unsigned)raw[i][0],
                    (unsigned)raw[i][1]);
      draw_cal_square(i, TFT_GREEN);
      delay(120);
    }
    if (!ok) {
      s_tft.setTextColor(TFT_RED, TFT_BLACK);
      s_tft.drawString("BATAL (timeout)", 160, 160, 4);
      delay(900);
      ui_show_page(2);
      return;
    }

    /* TFT_eSPI calibrateTouch() math from the 4 raw corner points. */
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

    /* Center tap must map back to the middle of the screen. */
    s_tft.fillScreen(TFT_BLACK);
    s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
    s_tft.drawString("ketuk titik TENGAH", 150, 6, 2);
    s_tft.drawCircle(240, 160, 32, TFT_YELLOW);
    s_tft.drawCircle(240, 160, 10, TFT_YELLOW);
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
    if (got) {
      Serial.printf("[touch] center maps to %u,%u\n", (unsigned)cx, (unsigned)cy);
      ok = (cx > 240 - 40 && cx < 240 + 40 && cy > 160 - 40 && cy < 160 + 40);
    } else {
      ok = false;
    }

    if (ok) {
      memcpy(g_settings.touch_cal, cal, sizeof cal);
      g_settings.touch_cal_valid = true;
      settings_save(&g_settings);
      Serial.printf("[touch] CALIB x0=%u x1=%u y0=%u y1=%u flags=%u (saved)\n", cal[0],
                    cal[1], cal[2], cal[3], cal[4]);
      s_tft.fillScreen(TFT_BLACK);
      s_tft.setTextColor(TFT_GREEN, TFT_BLACK);
      s_tft.drawString("KALIBRASI SELESAI", 130, 140, 4);
      delay(1200);
      ui_show_page(2);
      return;
    }

    if (g_settings.touch_cal_valid) s_tft.setTouch(g_settings.touch_cal);
    s_tft.setTextColor(TFT_RED, TFT_BLACK);
    s_tft.drawString("TITIK TENGAH TIDAK PAS - ULANGI", 70, 280, 2);
    Serial.println("[touch] validation failed, retrying");
    delay(1500);
  }

  if (g_settings.touch_cal_valid) s_tft.setTouch(g_settings.touch_cal);
  s_tft.fillScreen(TFT_BLACK);
  s_tft.setTextColor(TFT_RED, TFT_BLACK);
  s_tft.drawString("KALIBRASI GAGAL", 170, 140, 4);
  delay(1200);
  ui_show_page(2);
}
void touch_calibrate_now(void) { run_touch_calibration(); }

/* ------------------------------------------------------------------------- */
/*  RGB status LED (active LOW)                                              */
/* ------------------------------------------------------------------------- */
static void led_write(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? LOW : HIGH);
  digitalWrite(PIN_LED_G, g ? LOW : HIGH);
  digitalWrite(PIN_LED_B, b ? LOW : HIGH);
}

static void status_led(void) {
  if (millis() - s_last_led < 500) return;
  s_last_led = millis();
  if (wifi_mgr_portal_active()) {
    led_write(false, false, true); /* blue: setup portal */
  } else if (mqtt_mgr_connected()) {
    led_write(false, true, false); /* green: everything up */
  } else if (wifi_mgr_state() == WIFI_ST_OK) {
    led_write(true, true, false); /* amber: Wi-Fi ok, MQTT down */
  } else {
    led_write(true, false, false); /* red: no Wi-Fi */
  }
}

/* ------------------------------------------------------------------------- */
/*  Serial provisioning console (alternative to the captive portal)          */
/*    help | status | wifi <ssid> <pass> | mqtt <host> <port> <user> <pass>   */
/*    floor <label...> | dev <id> | save (reboot) | reboot                    */
/* ------------------------------------------------------------------------- */
static char s_cmd_buf[160];
static size_t s_cmd_len = 0;

static void serial_help(void) {
  Serial.println("commands:");
  Serial.println("  status                     print current settings");
  Serial.println("  wifi <ssid> <pass>         set Wi-Fi credentials");
  Serial.println("  mqtt <host> <port> <user> <pass>");
  Serial.println("  ctrl <host> <port>         HIOTO controller HTTP API (registry)");
  Serial.println("  floor <label...>           e.g. floor Lantai 1");
  Serial.println("  floorid <n>                controller floor id (3 = Lantai 1)");
  Serial.println("  dev <id>                   override device id");
  Serial.println("  ldr                        print raw LDR reading");
  Serial.println("  reg                        refetch the floor registry");
  Serial.println("  cal                        run touch calibration (tap crosses)");
  Serial.println("  traw                       raw touch debug (10 s)");
  Serial.println("  clearcal                   drop saved calibration, reboot");
  Serial.println("  getcal                     print current calibration values");
  Serial.println("  save                       persist to NVS and reboot");
  Serial.println("  reboot");
}

static void serial_status(void) {
  Serial.printf("dev=%s floor=%s bright=%u\n", g_settings.device_id, g_settings.floor_label,
                (unsigned)g_settings.brightness);
  Serial.printf("wifi_ssid=%s\n", g_settings.wifi_ssid);
  Serial.printf("mqtt=%s:%u user=%s\n", g_settings.mqtt_host, (unsigned)g_settings.mqtt_port,
                g_settings.mqtt_user);
}

static void serial_handle_line(char *line) {
  char *tok[5];
  int n = 0;
  char *save = NULL;
  for (char *t = strtok_r(line, " \t\r", &save); t && n < 5; t = strtok_r(NULL, " \t\r", &save))
    tok[n++] = t;
  if (n == 0) return;

  if (!strcmp(tok[0], "help")) {
    serial_help();
  } else if (!strcmp(tok[0], "status")) {
    serial_status();
  } else if (!strcmp(tok[0], "wifi") && n >= 2) {
    strncpy(g_settings.wifi_ssid, tok[1], sizeof g_settings.wifi_ssid - 1);
    g_settings.wifi_ssid[sizeof g_settings.wifi_ssid - 1] = '\0';
    g_settings.wifi_pass[0] = '\0';
    if (n >= 3) {
      strncpy(g_settings.wifi_pass, tok[2], sizeof g_settings.wifi_pass - 1);
      g_settings.wifi_pass[sizeof g_settings.wifi_pass - 1] = '\0';
    }
    Serial.printf("[cfg] wifi ssid=%s\n", g_settings.wifi_ssid);
  } else if (!strcmp(tok[0], "mqtt") && n >= 3) {
    strncpy(g_settings.mqtt_host, tok[1], sizeof g_settings.mqtt_host - 1);
    g_settings.mqtt_host[sizeof g_settings.mqtt_host - 1] = '\0';
    g_settings.mqtt_port = (uint16_t)atoi(tok[2]);
    if (n >= 4) {
      strncpy(g_settings.mqtt_user, tok[3], sizeof g_settings.mqtt_user - 1);
      g_settings.mqtt_user[sizeof g_settings.mqtt_user - 1] = '\0';
    }
    g_settings.mqtt_pass[0] = '\0';
    if (n >= 5) {
      strncpy(g_settings.mqtt_pass, tok[4], sizeof g_settings.mqtt_pass - 1);
      g_settings.mqtt_pass[sizeof g_settings.mqtt_pass - 1] = '\0';
    }
    Serial.printf("[cfg] mqtt %s:%u\n", g_settings.mqtt_host, (unsigned)g_settings.mqtt_port);
  } else if (!strcmp(tok[0], "ctrl") && n >= 2) {
    strncpy(g_settings.ctrl_host, tok[1], sizeof g_settings.ctrl_host - 1);
    g_settings.ctrl_host[sizeof g_settings.ctrl_host - 1] = '\0';
    if (n >= 3) g_settings.ctrl_port = (uint16_t)atoi(tok[2]);
    registry_begin();
    Serial.printf("[cfg] ctrl http://%s:%u\n", g_settings.ctrl_host,
                  (unsigned)g_settings.ctrl_port);
  } else if (!strcmp(tok[0], "floorid") && n >= 2) {
    g_settings.floor_id = (uint8_t)atoi(tok[1]);
    registry_begin();
    Serial.printf("[cfg] floor_id=%u\n", (unsigned)g_settings.floor_id);
  } else if (!strcmp(tok[0], "ldr")) {
    int adc = analogRead(PIN_LDR);
    int pct = (adc - 300) * 100 / (3900 - 300);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    Serial.printf("[ldr] adc=%d pct=%d%%\n", adc, pct);
  } else if (!strcmp(tok[0], "reg")) {
    registry_begin();
    Serial.println("[reg] will refetch registry");
  } else if (!strcmp(tok[0], "cal")) {
    touch_calibrate_now();
  } else if (!strcmp(tok[0], "clearcal")) {
    Preferences prefs;
    if (prefs.begin("hioto", false)) {
      prefs.remove("tcal");
      prefs.end();
    }
    g_settings.touch_cal_valid = false;
    Serial.println("[cfg] touch calibration cleared; rebooting with default mapping");
    delay(300);
    ESP.restart();
  } else if (!strcmp(tok[0], "getcal")) {
    Serial.printf("[cfg] tcal valid=%d x0=%u x1=%u y0=%u y1=%u flags=%u\n",
                  g_settings.touch_cal_valid, g_settings.touch_cal[0],
                  g_settings.touch_cal[1], g_settings.touch_cal[2],
                  g_settings.touch_cal[3], g_settings.touch_cal[4]);
  } else if (!strcmp(tok[0], "traw")) {
    Serial.println("[traw] tap near the 4 corners (10 s)...");
    uint32_t t0 = millis();
    while (millis() - t0 < 10000) {
      uint16_t rz = s_tft.getTouchRawZ();
      if (rz > 200) {
        uint16_t tx, ty;
        s_tft.getTouchRaw(&tx, &ty);
        Serial.printf("[traw] z=%u raw=%u,%u\n", (unsigned)rz, (unsigned)tx, (unsigned)ty);
        delay(80);
      } else {
        delay(10);
      }
    }
    Serial.println("[traw] done");
  } else if (!strcmp(tok[0], "floor") && n >= 2) {
    char label[24] = "";
    for (int i = 1; i < n; i++) {
      if (i > 1) strncat(label, " ", sizeof label - strlen(label) - 1);
      strncat(label, tok[i], sizeof label - strlen(label) - 1);
    }
    strncpy(g_settings.floor_label, label, sizeof g_settings.floor_label - 1);
    g_settings.floor_label[sizeof g_settings.floor_label - 1] = '\0';
    Serial.printf("[cfg] floor=%s\n", g_settings.floor_label);
  } else if (!strcmp(tok[0], "dev") && n >= 2) {
    strncpy(g_settings.device_id, tok[1], sizeof g_settings.device_id - 1);
    g_settings.device_id[sizeof g_settings.device_id - 1] = '\0';
    Serial.printf("[cfg] dev=%s\n", g_settings.device_id);
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
    Serial.printf("[cfg] unknown command '%s' (try 'help')\n", tok[0]);
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
  digitalWrite(PIN_LCD_BL, LOW); /* keep the panel dark during bring-up */
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  led_write(false, false, false);

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== HIOTO Floor Monitor / Type F / v1.0 ===");
  Serial.printf("[boot] chip=%s rev=%u cores=%u flash=%uKB psram=%uKB\n", ESP.getChipModel(),
                (unsigned)ESP.getChipRevision(), (unsigned)ESP.getChipCores(),
                (unsigned)(ESP.getFlashChipSize() / 1024),
                (unsigned)(ESP.getPsramSize() / 1024));

  lv_init();
  s_tft.init();
  s_tft.setRotation(1); /* landscape 480x320 */
  s_tft.fillScreen(TFT_BLACK);

  settings_load(&g_settings);
  ui_backlight_set(g_settings.brightness);
  Serial.printf("[cfg] dev=%s floor=%s bright=%u mqtt=%s:%u\n", g_settings.device_id,
                g_settings.floor_label, (unsigned)g_settings.brightness, g_settings.mqtt_host,
                g_settings.mqtt_port);

  const uint32_t px = LCD_WIDTH * kDrawLines;
  s_buf_a = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s_buf_b = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_buf_a || !s_buf_b) {
    Serial.println("[boot] FATAL: no RAM for LVGL draw buffers");
    while (true) delay(1000);
  }

  lv_disp_draw_buf_init(&s_draw_buf, s_buf_a, s_buf_b, px);
  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = LCD_WIDTH;
  s_disp_drv.ver_res = LCD_HEIGHT;
  s_disp_drv.flush_cb = lv_flush_cb;
  s_disp_drv.draw_buf = &s_draw_buf;
  lv_disp_drv_register(&s_disp_drv);

  /* touch (with saved calibration if present) */
  if (g_settings.touch_cal_valid) {
    s_tft.setTouch(g_settings.touch_cal);
    Serial.println("[touch] saved calibration applied");
  }
  static lv_indev_drv_t s_indev_drv;
  lv_indev_drv_init(&s_indev_drv);
  s_indev_drv.type = LV_INDEV_TYPE_POINTER;
  s_indev_drv.read_cb = indev_read_cb;
  lv_indev_drv_register(&s_indev_drv);

  monitor_init();
  registry_begin();
  ui_init();
  ui_set_cal_hook(run_touch_calibration);

  wifi_mgr_begin(&g_settings);
  mqtt_mgr_begin(&g_settings);

  pinMode(PIN_BOOT, INPUT_PULLUP);
  Serial.println("[ui] ready");
}

void loop() {
  lv_timer_handler();
  wifi_mgr_poll();
  mqtt_mgr_poll();
  monitor_poll();
  if (registry_poll()) {
    Serial.printf("[reg] ready: %s\n", registry_status());
    monitor_prune();
  }
  serial_poll();
  status_led();

  if (millis() - s_last_ui > 250) {
    s_last_ui = millis();
    ui_refresh();
  }

  /* BOOT button: short press cycles pages; hold >= 4 s = factory reset. */
  const bool btn = (digitalRead(PIN_BOOT) == LOW);
  if (btn && !s_btn_prev) {
    s_btn_pressed_at = millis();
    s_btn_long_done = false;
    ui_next_page();
    delay(30);
  } else if (btn && !s_btn_long_done && millis() - s_btn_pressed_at > 4000) {
    s_btn_long_done = true;
    Serial.println("[boot] factory reset (hold) — clearing NVS");
    settings_factory_reset();
    delay(200);
    ESP.restart();
  }
  s_btn_prev = btn;

  delay(5);
}
