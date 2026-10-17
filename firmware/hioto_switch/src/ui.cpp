#include <Arduino.h>
#include <lvgl.h>

#include "board_pins.h"
#include "config.h"
#include "relay.h"
#include "settings.h"
#include "mqtt_mgr.h"
#include "ui.h"

extern void ui_backlight_set(int pct); /* main.cpp */
extern Settings g_settings;

#define COL_BG     0x0B1220
#define COL_CARD   0x111A2E
#define COL_BORDER 0x1E2A44
#define COL_TEXT   0xE2E8F0
#define COL_MUTED  0x7A8AA0
#define COL_ACCENT 0x3B82F6
#define COL_GREEN  0x22C55E
#define COL_OFF    0x475569
#define COL_AMBER  0xF59E0B
#define COL_RED    0xEF4444

static lv_obj_t *s_sw = NULL;
static lv_obj_t *s_lbl_state = NULL;
static lv_obj_t *s_led = NULL;
static lv_obj_t *s_slider = NULL;
static lv_obj_t *s_lbl_bright = NULL;
static lv_obj_t *s_lbl_net = NULL;
static lv_obj_t *s_lbl_footer = NULL;
static bool s_syncing = false;

static lv_obj_t *make_label(lv_obj_t *parent, const char *txt,
                            const lv_font_t *font, uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  return l;
}

static void apply_visual(bool on) {
  lv_label_set_text(s_lbl_state, on ? "ON" : "OFF");
  lv_obj_set_style_text_color(s_lbl_state, lv_color_hex(on ? COL_GREEN : COL_OFF), 0);
  if (on) lv_led_on(s_led); else lv_led_off(s_led);
}

static void cb_switch(lv_event_t *e) {
  (void)e;
  if (s_syncing) return;
  const bool on = lv_obj_has_state(s_sw, LV_STATE_CHECKED);
  relay_set(on);
  apply_visual(on);
  mqtt_mgr_publish_state();
}

static void cb_bright(lv_event_t *e) {
  (void)e;
  int v = lv_slider_get_value(s_slider);
  if (v < 20) { v = 20; lv_slider_set_value(s_slider, 20, LV_ANIM_OFF); }
  g_settings.brightness = (uint8_t)v;
  ui_backlight_set(v);
  lv_label_set_text_fmt(s_lbl_bright, "%d%%", v);
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save(&g_settings);
}

static void set_switch_silently(bool on) {
  s_syncing = true;
  if (on) lv_obj_add_state(s_sw, LV_STATE_CHECKED);
  else lv_obj_clear_state(s_sw, LV_STATE_CHECKED);
  s_syncing = false;
}

void ui_toggle_switch(void) {
  const bool on = !lv_obj_has_state(s_sw, LV_STATE_CHECKED);
  set_switch_silently(on);
  relay_set(on);
  apply_visual(on);
  mqtt_mgr_publish_state();
  Serial.printf("[relay] output -> %s (BOOT)\n", on ? "ON" : "OFF");
}

void ui_apply_external_state(bool on) {
  set_switch_silently(on);
  apply_visual(on);
}

void ui_set_brightness(uint8_t pct) {
  if (pct < 20) pct = 20;
  if (pct > 100) pct = 100;
  lv_slider_set_value(s_slider, pct, LV_ANIM_OFF); /* triggers cb_bright visuals */
}

void ui_set_net_status(const char *status_line, const char *footer_line) {
  if (s_lbl_net) lv_label_set_text(s_lbl_net, status_line ? status_line : "");
  if (s_lbl_footer) {
    if (footer_line && footer_line[0]) lv_label_set_text(s_lbl_footer, footer_line);
    else lv_label_set_text(s_lbl_footer, g_settings.device_id);
  }
}

void ui_init(void) {
  lv_theme_default_init(lv_disp_get_default(),
                        lv_color_hex(COL_ACCENT), lv_color_hex(COL_GREEN),
                        true, LV_FONT_DEFAULT);

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(scr, 6, 0);

  /* ---- header ---- */
  make_label(scr, "HIOTO", &lv_font_montserrat_28, COL_TEXT);
  make_label(scr, "SECURED SWITCH \xE2\x80\xA2 TYPE A", &lv_font_montserrat_14, COL_MUTED);

  /* ---- network status line ---- */
  s_lbl_net = make_label(scr, "booting\xE2\x80\xA6", &lv_font_montserrat_14, COL_MUTED);
  lv_obj_set_width(s_lbl_net, 160);
  lv_label_set_long_mode(s_lbl_net, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(s_lbl_net, LV_TEXT_ALIGN_CENTER, 0);

  /* ---- main switch card ---- */
  lv_obj_t *card = lv_obj_create(scr);
  lv_obj_set_size(card, 150, 140);
  lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(COL_BORDER), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 16, 0);
  lv_obj_set_style_pad_all(card, 10, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(card, 6, 0);

  s_led = lv_led_create(card);
  lv_obj_set_size(s_led, 10, 10);
  lv_led_set_color(s_led, lv_color_hex(COL_GREEN));
  lv_led_off(s_led);

  s_lbl_state = make_label(card, "OFF", &lv_font_montserrat_20, COL_OFF);

  s_sw = lv_switch_create(card);
  lv_obj_set_size(s_sw, 100, 48);
  lv_obj_add_event_cb(s_sw, cb_switch, LV_EVENT_VALUE_CHANGED, NULL);

  /* ---- brightness ---- */
  lv_obj_t *bsp = lv_obj_create(scr);
  lv_obj_set_size(bsp, 168, 50);
  lv_obj_set_style_bg_opa(bsp, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bsp, 0, 0);
  lv_obj_clear_flag(bsp, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(bsp, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(bsp, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(bsp, 6, 0);

  lv_obj_t *brow = lv_obj_create(bsp);
  lv_obj_set_size(brow, 168, 18);
  lv_obj_set_style_bg_opa(brow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(brow, 0, 0);
  lv_obj_clear_flag(brow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(brow, 0, 0);
  make_label(brow, "BRIGHTNESS", &lv_font_montserrat_14, COL_MUTED);
  s_lbl_bright = make_label(brow, "80%", &lv_font_montserrat_14, COL_TEXT);

  s_slider = lv_slider_create(bsp);
  lv_obj_set_width(s_slider, 150);
  lv_slider_set_range(s_slider, 20, 100);
  lv_slider_set_value(s_slider, g_settings.brightness, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(s_slider, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
  lv_obj_add_event_cb(s_slider, cb_bright, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(s_slider, cb_bright, LV_EVENT_RELEASED, NULL);
  lv_label_set_text_fmt(s_lbl_bright, "%u%%", (unsigned)g_settings.brightness);

  /* ---- footer ---- */
  s_lbl_footer = make_label(scr, g_settings.device_id, &lv_font_montserrat_14, COL_MUTED);

  apply_visual(relay_get());
}
