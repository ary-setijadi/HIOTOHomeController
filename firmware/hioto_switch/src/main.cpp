/**
 * HIOTO Secured Switch — Type A · v1.0
 * ---------------------------------------------------------------------------
 *  Board: Sunton ESP32-1732S019C
 *    - ESP32-S3-WROOM-1 (N16): 16 MB flash (no PSRAM on this unit)
 *    - ST7789 170x320 SPI display, GT911 I2C touch (auto-probed)
 *    - CH340 USB-UART (COM3) — serial console @115200
 *
 *  Milestone 2 — switch-device skeleton:
 *    - NVS-backed settings (settings.cpp)
 *    - Wi-Fi provisioning captive portal + station mode (wifi_mgr.cpp)
 *    - MQTT client: iot/{id}/telemetry|commands|status (mqtt_mgr.cpp)
 *    - relay/switch output driver stub, GPIO configurable (relay.cpp)
 *    - LVGL UI: switch, brightness, network status (ui.cpp)
 *
 *  Hardware note: no relay is wired on the basic board yet (-1 = simulated);
 *  output GPIO is configurable in NVS via the provisioning portal.
 */
#include <Arduino.h>
#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>
#include <TFT_eSPI.h>
#include <lvgl.h>

#include "board_pins.h"
#include "config.h"
#include "settings.h"
#include "relay.h"
#include "wifi_mgr.h"
#include "mqtt_mgr.h"
#include "touch.h"
#include "ui.h"

static TFT_eSPI s_tft;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;

static const uint32_t kDrawLines = 40;   /* horizontal lines per LVGL draw buffer */
static lv_color_t *s_buf_a = nullptr;
static lv_color_t *s_buf_b = nullptr;

static uint8_t s_backlight_pct = 80;
static bool s_btn_prev = false;
static uint32_t s_btn_pressed_at = 0;
static bool s_btn_long_done = false;
static uint32_t s_status_last = 0;

/* ------------------------------------------------------------------------- */
/*  Backlight — PWM on the display BL pin (active HIGH on this board).       */
/* ------------------------------------------------------------------------- */
void ui_backlight_set(int pct) {
  static bool s_ledc_ready = false;
  if (pct < 1) pct = 1;
  if (pct > 100) pct = 100;
  s_backlight_pct = (uint8_t)pct;
  if (!s_ledc_ready) {
    ledcSetup(0, 25000, 10);          /* 25 kHz, 10-bit — inaudible, smooth */
    ledcAttachPin(PIN_LCD_BL, 0);
    s_ledc_ready = true;
  }
  ledcWrite(0, (uint32_t)s_backlight_pct * 1023U / 100U);
}

/* ------------------------------------------------------------------------- */
/*  LVGL → TFT_eSPI flush                                                    */
/* ------------------------------------------------------------------------- */
static void lv_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  const uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
  const uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
  s_tft.startWrite();
  s_tft.setAddrWindow(area->x1, area->y1, w, h);
  s_tft.pushColors((uint16_t *)&color_p->full, w * h, true); /* true = byte swap for ST7789 */
  s_tft.endWrite();
  lv_disp_flush_ready(disp);
}

static lv_color_t *alloc_draw_buf(uint32_t px) {
  lv_color_t *p =
      (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  if (p) return p;
  return (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL);
}

/* ------------------------------------------------------------------------- */
static void update_status_line(void) {
  if (millis() - s_status_last < 250) return;
  s_status_last = millis();

  static const char *l1 = "";
  static char buf[48];
  static char foot[24];

  const WifiState ws = wifi_mgr_state();
  foot[0] = '\0';
  if (ws == WIFI_ST_PORTAL) {
    l1 = "AP: HIOTO-Setup";
    strcpy(foot, "192.168.4.1");
  } else if (ws == WIFI_ST_OK) {
    snprintf(buf, sizeof buf, "WiFi OK \xC2\xB7 MQTT %s", mqtt_mgr_connected() ? "OK" : "\xE2\x80\xA6");
    l1 = buf;
    strlcpy(foot, wifi_mgr_ip().c_str(), sizeof foot);
  } else if (ws == WIFI_ST_CONNECTING) {
    l1 = "WiFi: connecting\xE2\x80\xA6";
  } else {
    l1 = "WiFi: off";
  }
  ui_set_net_status(l1, foot);
}

void setup() {
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW); /* keep the panel dark during bring-up */

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== HIOTO Secured Switch / Type A / v1.0 ===");
  Serial.printf("[boot] chip=%s rev=%u cores=%u flash=%uKB psram=%uKB\n",
                ESP.getChipModel(), (unsigned)ESP.getChipRevision(),
                (unsigned)ESP.getChipCores(),
                (unsigned)(ESP.getFlashChipSize() / 1024),
                (unsigned)(ESP.getPsramSize() / 1024));

  lv_init();
  s_tft.init();
  s_tft.setRotation(0);
  s_tft.fillScreen(TFT_BLACK);

  settings_load(&g_settings);
  s_backlight_pct = g_settings.brightness;
  ui_backlight_set(s_backlight_pct);
  Serial.printf("[cfg] dev=%s relay_gpio=%d bright=%u mqtt=%s:%u\n",
                g_settings.device_id, (int)g_settings.relay_gpio,
                (unsigned)g_settings.brightness, g_settings.mqtt_host, g_settings.mqtt_port);

  const uint32_t px = LCD_WIDTH * kDrawLines;
  s_buf_a = alloc_draw_buf(px);
  s_buf_b = alloc_draw_buf(px);

  lv_disp_draw_buf_init(&s_draw_buf, s_buf_a, s_buf_b, px);
  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = LCD_WIDTH;
  s_disp_drv.ver_res = LCD_HEIGHT;
  s_disp_drv.flush_cb = lv_flush_cb;
  s_disp_drv.draw_buf = &s_draw_buf;
  lv_disp_drv_register(&s_disp_drv);

  relay_init(g_settings.relay_gpio, RELAY_ACTIVE_LEVEL);

  const bool touch_ok = touch_init();
  Serial.printf("[input] %s\n", touch_ok ? touch_status_text()
                                         : "no GT911 touch — GPIO0 BOOT toggles the switch");

  ui_init();

  wifi_mgr_begin(&g_settings);
  mqtt_mgr_begin(&g_settings);

  pinMode(PIN_BOOT, INPUT_PULLUP);
  update_status_line();
  Serial.println("[ui] ready");
}

void loop() {
  lv_timer_handler();
  wifi_mgr_poll();
  mqtt_mgr_poll();
  update_status_line();

  /* BOOT button: short press toggles the switch; hold >= 4 s = factory reset
   * (clears NVS settings and reboots into the provisioning portal). */
  const bool btn = (digitalRead(PIN_BOOT) == LOW);
  if (btn && !s_btn_prev) {
    s_btn_pressed_at = millis();
    s_btn_long_done = false;
    ui_toggle_switch();
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
