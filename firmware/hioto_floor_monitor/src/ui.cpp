#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>

#include "board_pins.h"
#include "config.h"
#include "settings.h"
#include "monitor.h"
#include "mqtt_mgr.h"
#include "wifi_mgr.h"
#include "registry.h"
#include "ui.h"

extern void ui_backlight_set(int pct); /* main.cpp */

#define COL_BG     0x0B1220
#define COL_CARD   0x111A2E
#define COL_CARD2  0x0E1626
#define COL_BORDER 0x1E2A44
#define COL_TEXT   0xE2E8F0
#define COL_MUTED  0x7A8AA0
#define COL_DIM    0x4A5A70
#define COL_ACCENT 0x3B82F6
#define COL_GREEN  0x22C55E
#define COL_AMBER  0xF59E0B
#define COL_RED    0xEF4444
#define COL_CYAN   0x22D3EE

/* Views: 0 = dashboard, 1 = node detail, 2 = system info */
static lv_obj_t *s_views[3] = {NULL, NULL, NULL};
static int s_page = 0;

/* dashboard widgets */
static lv_obj_t *s_lbl_floor = NULL;
static lv_obj_t *s_lbl_net = NULL;
static lv_obj_t *s_lbl_summary = NULL;
static lv_obj_t *s_lbl_uptime = NULL;
static lv_obj_t *s_row[MON_ROWS_MAX];
static lv_obj_t *s_row_name[MON_ROWS_MAX];
static lv_obj_t *s_row_val[MON_ROWS_MAX];
static lv_obj_t *s_row_age[MON_ROWS_MAX];
static lv_obj_t *s_row_toggle[MON_ROWS_MAX];   /* lamp on/off pill (tappable) */
static lv_obj_t *s_row_tog_lbl[MON_ROWS_MAX];
static int s_row_sel[MON_ROWS_MAX]; /* row -> monitor node index */

/* room tabs */
static lv_obj_t *s_tab_bar = NULL;
static int s_sel_room = -1;      /* -1 = Semua */
static int s_tab_roomcount = -1;
static bool s_tabs_dirty = false;
static lv_obj_t *s_lbl_more = NULL;
static lv_obj_t *s_lbl_env[4];   /* suhu, cahaya, sinyal, memori */
static lv_obj_t *s_lbl_net2[3];  /* wifi, mqtt, ip */

/* detail widgets */
static lv_obj_t *s_det_title = NULL;
static lv_obj_t *s_det_status = NULL;
static lv_obj_t *s_det_value = NULL;
static lv_obj_t *s_det_meta = NULL;
static char s_det_id[40];

/* info widgets */
static lv_obj_t *s_inf_line[9];
static ui_cal_hook_t s_cal_hook = NULL;

/* ------------------------------------------------------------------------- */
static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                          uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  return l;
}

static lv_obj_t *mk_card(lv_obj_t *parent, int w, int h) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
  lv_obj_set_style_border_color(c, lv_color_hex(COL_BORDER), 0);
  lv_obj_set_style_border_width(c, 1, 0);
  lv_obj_set_style_radius(c, 10, 0);
  lv_obj_set_style_pad_all(c, 8, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

static void show_page(int p) {
  if (p < 0 || p > 2) p = 0;
  s_page = p;
  for (int i = 0; i < 3; i++) {
    if (s_views[i]) {
      if (i == p) lv_obj_clear_flag(s_views[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(s_views[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void ui_show_page(int page) { show_page(page); }
int ui_page(void) { return s_page; }

/* ------------------------------------------------------------------------- */
/*  Dashboard                                                                 */
/* ------------------------------------------------------------------------- */
static void toggle_click_cb(lv_event_t *e) {
  lv_event_stop_bubbling(e);
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= MON_ROWS_MAX) return;
  MonNode *n = monitor_node(s_row_sel[idx]);
  if (!n || !n->actuator) return;
  /* HIOTO lamps/relays are ACTIVE-LOW: 0 = ON, 1 = OFF */
  const bool on = !strcmp(n->value, "ON");
  mqtt_publish_actuator(n->id, on ? 1 : 0);
  snprintf(n->value, sizeof n->value, "%s", on ? "OFF" : "ON"); /* optimistic */
  ui_refresh();
}

static void row_click_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= MON_ROWS_MAX) return;
  MonNode *n = monitor_node(s_row_sel[idx]);
  if (n) ui_detail_select(n->id);
}

static void mk_row(lv_obj_t *parent, int i) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_size(row, LV_PCT(100), 46);
  lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD2), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_radius(row, 8, 0);
  lv_obj_set_style_pad_all(row, 4, 0);
  lv_obj_set_style_pad_column(row, 6, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

  s_row_name[i] = mk_label(row, "-", &lv_font_montserrat_14, COL_TEXT);
  lv_obj_set_width(s_row_name[i], 96);
  lv_label_set_long_mode(s_row_name[i], LV_LABEL_LONG_CLIP);

  s_row_val[i] = mk_label(row, "-", &lv_font_montserrat_12, COL_MUTED);
  lv_obj_set_width(s_row_val[i], 48);
  lv_label_set_long_mode(s_row_val[i], LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_align(s_row_val[i], LV_TEXT_ALIGN_RIGHT, 0);

  s_row_age[i] = mk_label(row, "-", &lv_font_montserrat_12, COL_DIM);
  lv_obj_set_width(s_row_age[i], 34);
  lv_obj_set_style_text_align(s_row_age[i], LV_TEXT_ALIGN_RIGHT, 0);

  /* tappable ON/OFF pill for lamp/actuator rows (toggles the lamp) */
  s_row_toggle[i] = lv_btn_create(row);
  lv_obj_set_size(s_row_toggle[i], 58, 32);
  lv_obj_set_style_radius(s_row_toggle[i], 16, 0);
  lv_obj_set_style_bg_color(s_row_toggle[i], lv_color_hex(COL_DIM), 0);
  lv_obj_set_style_bg_opa(s_row_toggle[i], LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_row_toggle[i], 0, 0);
  lv_obj_set_style_pad_all(s_row_toggle[i], 0, 0);
  lv_obj_add_event_cb(s_row_toggle[i], toggle_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  s_row_tog_lbl[i] = mk_label(s_row_toggle[i], "-", &lv_font_montserrat_12, COL_TEXT);
  lv_obj_center(s_row_tog_lbl[i]);
  lv_obj_add_flag(s_row_toggle[i], LV_OBJ_FLAG_HIDDEN);

  lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
  s_row[i] = row;
}

/* ------------------------------------------------------------------------- */
/*  Room tabs                                                                 */
/* ------------------------------------------------------------------------- */
static void build_tabs(void);

static void tab_click_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  s_sel_room = idx;
  s_tabs_dirty = true; /* rebuild + refilter on the next ui_refresh() */
}

static void build_tabs(void) {
  if (!s_tab_bar) return;
  int rn = registry_room_count();
  if (s_sel_room >= rn) s_sel_room = -1;
  lv_obj_clean(s_tab_bar);

  lv_obj_t *b = lv_btn_create(s_tab_bar);
  lv_obj_set_size(b, 74, 28);
  lv_obj_set_style_bg_color(b, lv_color_hex(s_sel_room < 0 ? COL_ACCENT : COL_CARD), 0);
  lv_obj_set_style_pad_all(b, 2, 0);
  lv_obj_add_event_cb(b, tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
  mk_label(b, "Semua", &lv_font_montserrat_12, COL_TEXT);

  /* real rooms first, "Lainnya" last */
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < rn; i++) {
      const char *name = registry_room_name(i);
      if (!name) continue;
      bool is_other = !strcmp(name, "Lainnya");
      if ((pass == 0 && is_other) || (pass == 1 && !is_other)) continue;
      lv_obj_t *bt = lv_btn_create(s_tab_bar);
      lv_obj_set_size(bt, 132, 28);
      lv_obj_set_style_bg_color(bt, lv_color_hex(i == s_sel_room ? COL_ACCENT : COL_CARD), 0);
      lv_obj_set_style_pad_all(bt, 2, 0);
      lv_obj_add_event_cb(bt, tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
      mk_label(bt, name, &lv_font_montserrat_12, COL_TEXT);
    }
  }
}

static void mk_dashboard(lv_obj_t *root) {
  lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(root, 6, 0);

  /* ---- top bar ---- */
  lv_obj_t *top = lv_obj_create(root);
  lv_obj_set_size(top, LV_PCT(100), 34);
  lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(top, 0, 0);
  lv_obj_set_style_pad_all(top, 0, 0);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(top, 10, 0);
  lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

  mk_label(top, "HIOTO", &lv_font_montserrat_20, COL_TEXT);
  s_lbl_floor = mk_label(top, g_settings.floor_label, &lv_font_montserrat_20, COL_ACCENT);
  lv_obj_t *spacer = lv_obj_create(top);
  lv_obj_set_width(spacer, 40);
  lv_obj_set_height(spacer, 1);
  lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spacer, 0, 0);
  lv_obj_set_flex_grow(spacer, 1);
  s_lbl_net = mk_label(top, "menghubungkan...", &lv_font_montserrat_12, COL_MUTED);

  /* ---- middle: node list + environment column ---- */
  lv_obj_t *mid = lv_obj_create(root);
  lv_obj_set_width(mid, LV_PCT(100));
  lv_obj_set_flex_grow(mid, 1);
  lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(mid, 0, 0);
  lv_obj_set_style_pad_all(mid, 0, 0);
  lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(mid, 6, 0);
  lv_obj_clear_flag(mid, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *list = mk_card(mid, 296, LV_PCT(100));
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 4, 0);

  /* room tab bar (scrolls horizontally when there are many rooms) */
  s_tab_bar = lv_obj_create(list);
  lv_obj_set_size(s_tab_bar, LV_PCT(100), 30);
  lv_obj_set_style_bg_opa(s_tab_bar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_tab_bar, 0, 0);
  lv_obj_set_style_pad_all(s_tab_bar, 0, 0);
  lv_obj_set_style_pad_column(s_tab_bar, 4, 0);
  lv_obj_set_flex_flow(s_tab_bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_tab_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(s_tab_bar, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(s_tab_bar, LV_SCROLLBAR_MODE_OFF);
  build_tabs();

  lv_obj_t *rows = lv_obj_create(list);
  lv_obj_set_size(rows, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(rows, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(rows, 0, 0);
  lv_obj_set_style_pad_all(rows, 0, 0);
  lv_obj_set_style_pad_row(rows, 4, 0);
  lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(rows, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(rows, LV_SCROLLBAR_MODE_AUTO);
  for (int i = 0; i < MON_ROWS_MAX; i++) mk_row(rows, i);
  s_lbl_more = mk_label(rows, "", &lv_font_montserrat_12, COL_DIM);

  lv_obj_t *env = lv_obj_create(mid);
  lv_obj_set_width(env, 160);
  lv_obj_set_height(env, LV_PCT(100));
  lv_obj_set_style_bg_opa(env, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(env, 0, 0);
  lv_obj_set_style_pad_all(env, 0, 0);
  lv_obj_set_style_pad_row(env, 6, 0);
  lv_obj_set_flex_flow(env, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(env, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *envc = mk_card(env, 160, 128);
  lv_obj_set_flex_flow(envc, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(envc, 2, 0);
  mk_label(envc, "LINGKUNGAN", &lv_font_montserrat_12, COL_MUTED);
  static const char *k_env_txt[4] = {"Suhu  --", "Cahaya  --", "Sinyal  --", "Memori  --"};
  for (int i = 0; i < 4; i++) s_lbl_env[i] = mk_label(envc, k_env_txt[i], &lv_font_montserrat_14, COL_TEXT);

  lv_obj_t *netc = mk_card(env, 160, 96);
  lv_obj_set_flex_flow(netc, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(netc, 2, 0);
  mk_label(netc, "STATUS", &lv_font_montserrat_12, COL_MUTED);
  static const char *k_net_txt[3] = {"WiFi  --", "MQTT  --", "IP  --"};
  for (int i = 0; i < 3; i++) s_lbl_net2[i] = mk_label(netc, k_net_txt[i], &lv_font_montserrat_12, COL_TEXT);

  /* ---- bottom bar ---- */
  lv_obj_t *bot = lv_obj_create(root);
  lv_obj_set_size(bot, LV_PCT(100), 26);
  lv_obj_set_style_bg_opa(bot, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bot, 0, 0);
  lv_obj_set_style_pad_all(bot, 0, 0);
  lv_obj_set_flex_flow(bot, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
  s_lbl_summary = mk_label(bot, "0 perangkat", &lv_font_montserrat_12, COL_MUTED);
  s_lbl_uptime = mk_label(bot, "uptime --", &lv_font_montserrat_12, COL_DIM);
}

/* ------------------------------------------------------------------------- */
/*  Node detail                                                               */
/* ------------------------------------------------------------------------- */
static void back_click_cb(lv_event_t *e) {
  (void)e;
  Serial.println("[ui] back clicked");
  show_page(0);
}

void ui_detail_select(const char *node_id) {
  strncpy(s_det_id, node_id, sizeof s_det_id - 1);
  s_det_id[sizeof s_det_id - 1] = '\0';
  show_page(1);
}

static void mk_detail(lv_obj_t *root) {
  lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(root, 4, 0);
  lv_obj_set_style_pad_row(root, 6, 0);

  lv_obj_t *bar = lv_obj_create(root);
  lv_obj_set_size(bar, LV_PCT(100), 26);
  lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 0, 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  mk_label(bar, "DETAIL PERANGKAT", &lv_font_montserrat_14, COL_MUTED);
  mk_label(bar, "BOOT = ganti halaman", &lv_font_montserrat_12, COL_DIM);

  lv_obj_t *card = mk_card(root, LV_PCT(100), 180);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 4, 0);

  s_det_title = mk_label(card, "-", &lv_font_montserrat_20, COL_TEXT);
  s_det_status = mk_label(card, "-", &lv_font_montserrat_14, COL_MUTED);
  mk_label(card, "NILAI TERAKHIR", &lv_font_montserrat_12, COL_MUTED);
  s_det_value = mk_label(card, "-", &lv_font_montserrat_28, COL_CYAN);
  lv_obj_set_width(s_det_value, LV_PCT(100));
  lv_label_set_long_mode(s_det_value, LV_LABEL_LONG_WRAP);
  s_det_meta = mk_label(card, "", &lv_font_montserrat_12, COL_DIM);
  lv_obj_set_width(s_det_meta, LV_PCT(100));
  lv_label_set_long_mode(s_det_meta, LV_LABEL_LONG_WRAP);

  /* back button sits in the safe interior zone (edges are outside the
   * resistive mapping until the panel is calibrated); the whole strip is
   * clickable as a fallback. */
  lv_obj_t *brow = lv_obj_create(root);
  lv_obj_set_size(brow, LV_PCT(100), 52);
  lv_obj_set_style_bg_opa(brow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(brow, 0, 0);
  lv_obj_set_style_pad_all(brow, 0, 0);
  lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(brow, 10, 0);
  lv_obj_clear_flag(brow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(brow, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(brow, back_click_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *back = lv_btn_create(brow);
  lv_obj_set_size(back, 320, 44);
  lv_obj_set_style_bg_color(back, lv_color_hex(COL_CARD), 0);
  lv_obj_add_event_cb(back, back_click_cb, LV_EVENT_CLICKED, NULL);
  mk_label(back, "<  Kembali", &lv_font_montserrat_16, COL_TEXT);
}

/* ------------------------------------------------------------------------- */
/*  System info page                                                          */
/* ------------------------------------------------------------------------- */
static void reboot_click_cb(lv_event_t *e) {
  (void)e;
  Serial.println("[ui] reboot clicked");
  delay(150);
  ESP.restart();
}

static void cal_click_cb(lv_event_t *e) {
  (void)e;
  Serial.println("[ui] calibrate clicked");
  if (s_cal_hook) s_cal_hook();
}

static void mk_info(lv_obj_t *root) {
  lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(root, 4, 0);
  lv_obj_set_style_pad_row(root, 4, 0);

  lv_obj_t *bar = lv_obj_create(root);
  lv_obj_set_size(bar, LV_PCT(100), 30);
  lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 0, 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  mk_label(bar, "SISTEM", &lv_font_montserrat_16, COL_TEXT);
  lv_obj_t *back = lv_btn_create(bar);
  lv_obj_set_size(back, 96, 26);
  lv_obj_set_style_bg_color(back, lv_color_hex(COL_CARD), 0);
  lv_obj_add_event_cb(back, back_click_cb, LV_EVENT_CLICKED, NULL);
  mk_label(back, "Kembali", &lv_font_montserrat_12, COL_TEXT);

  lv_obj_t *card = mk_card(root, LV_PCT(100), 176);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 2, 0);
  for (int i = 0; i < 9; i++)
    s_inf_line[i] = mk_label(card, "-", &lv_font_montserrat_12, COL_TEXT);

  lv_obj_t *btns = lv_obj_create(root);
  lv_obj_set_size(btns, LV_PCT(100), 46);
  lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btns, 0, 0);
  lv_obj_set_style_pad_all(btns, 0, 0);
  lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(btns, 8, 0);
  lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *bback = lv_btn_create(btns);
  lv_obj_set_size(bback, 120, 44);
  lv_obj_set_style_bg_color(bback, lv_color_hex(COL_CARD), 0);
  lv_obj_add_event_cb(bback, back_click_cb, LV_EVENT_CLICKED, NULL);
  mk_label(bback, "< Kembali", &lv_font_montserrat_14, COL_TEXT);

  lv_obj_t *bcal = lv_btn_create(btns);
  lv_obj_set_size(bcal, 170, 44);
  lv_obj_set_style_bg_color(bcal, lv_color_hex(COL_CARD), 0);
  lv_obj_add_event_cb(bcal, cal_click_cb, LV_EVENT_CLICKED, NULL);
  mk_label(bcal, "Kalibrasi Layar", &lv_font_montserrat_14, COL_TEXT);

  lv_obj_t *breb = lv_btn_create(btns);
  lv_obj_set_size(breb, 120, 44);
  lv_obj_set_style_bg_color(breb, lv_color_hex(0x3A1520), 0);
  lv_obj_add_event_cb(breb, reboot_click_cb, LV_EVENT_CLICKED, NULL);
  mk_label(breb, "Reboot", &lv_font_montserrat_14, COL_RED);
}

/* ------------------------------------------------------------------------- */
/*  Live updating                                                             */
/* ------------------------------------------------------------------------- */
static void format_uptime(char *buf, size_t n) {
  uint32_t s = millis() / 1000UL;
  uint32_t d = s / 86400UL;
  uint32_t h = (s % 86400UL) / 3600UL;
  uint32_t m = (s % 3600UL) / 60UL;
  if (d > 0) snprintf(buf, n, "%lud%02luh", (unsigned long)d, (unsigned long)h);
  else snprintf(buf, n, "%02luh%02lum", (unsigned long)h, (unsigned long)m);
}

static void update_rows(void) {
  const char *sel = (s_sel_room >= 0) ? registry_room_name(s_sel_room) : NULL;
  int matched = 0, shown = 0;
  for (int i = 0; i < monitor_count(); i++) {
    MonNode *node = monitor_node(i);
    if (!node) continue;
    if (sel && strcmp(node->room, sel)) continue;
    matched++;
    if (shown >= MON_ROWS_MAX) continue;
    s_row_sel[shown] = i;
    lv_obj_clear_flag(s_row[shown], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_row_name[shown], node->label);
    lv_label_set_text(s_row_val[shown], node->value);
    char age[16];
    uint32_t sec = (millis() - node->last_ms) / 1000UL;
    if (sec < 60) snprintf(age, sizeof age, "%lus", (unsigned long)sec);
    else snprintf(age, sizeof age, "%lum", (unsigned long)(sec / 60UL));
    lv_label_set_text(s_row_age[shown], age);
    lv_obj_set_style_text_color(s_row_name[shown], lv_color_hex(node->online ? COL_TEXT : COL_DIM), 0);

    /* lamp/actuator rows get a tappable ON/OFF pill on the right */
    if (node->actuator) {
      const bool on = !strcmp(node->value, "ON");
      lv_obj_clear_flag(s_row_toggle[shown], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_bg_color(s_row_toggle[shown], lv_color_hex(on ? COL_GREEN : COL_DIM), 0);
      lv_label_set_text(s_row_tog_lbl[shown], on ? "ON" : (node->value[0] == '-' ? "-" : "OFF"));
      lv_obj_set_style_text_color(s_row_val[shown], lv_color_hex(on ? COL_GREEN : COL_DIM), 0);
    } else {
      lv_obj_add_flag(s_row_toggle[shown], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_text_color(s_row_val[shown], lv_color_hex(COL_MUTED), 0);
    }
    shown++;
  }
  for (int i = shown; i < MON_ROWS_MAX; i++) lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);

  if (matched > shown) {
    lv_label_set_text_fmt(s_lbl_more, "%d dari %d perangkat (geser)", shown, matched);
    lv_obj_clear_flag(s_lbl_more, LV_OBJ_FLAG_HIDDEN);
  } else if (matched == 0) {
    lv_label_set_text(s_lbl_more, "tidak ada perangkat");
    lv_obj_clear_flag(s_lbl_more, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_lbl_more, LV_OBJ_FLAG_HIDDEN);
  }
}

static void update_detail(void) {
  MonNode *n = monitor_find(s_det_id);
  if (!n) {
    lv_label_set_text(s_det_title, "-");
    lv_label_set_text(s_det_status, "Perangkat tidak ditemukan");
    lv_label_set_text(s_det_value, "-");
    lv_label_set_text(s_det_meta, "");
    return;
  }
  lv_label_set_text(s_det_title, n->label);
  lv_label_set_text_fmt(s_det_status, "%s  (%s)", n->online ? "ONLINE" : "OFFLINE", n->kind);
  lv_obj_set_style_text_color(s_det_status, lv_color_hex(n->online ? COL_GREEN : COL_RED), 0);
  lv_label_set_text(s_det_value, n->value);
  char age[24];
  uint32_t sec = (millis() - n->last_ms) / 1000UL;
  if (sec < 60) snprintf(age, sizeof age, "terakhir %lus lalu", (unsigned long)sec);
  else snprintf(age, sizeof age, "terakhir %lum lalu", (unsigned long)(sec / 60UL));
  lv_label_set_text_fmt(s_det_meta, "id: %s\nroom: %s  ·  %s", n->id, n->room, age);
}

static void update_info(void) {
  char line[96];
  char up[24];
  format_uptime(up, sizeof up);
  const char *mst = mqtt_mgr_status_text();

  lv_label_set_text_fmt(s_inf_line[0], "Perangkat : %s", g_settings.device_id);
  lv_label_set_text_fmt(s_inf_line[1], "Lantai    : %s", g_settings.floor_label);
  lv_label_set_text_fmt(s_inf_line[2], "WiFi      : %s  (%s)", g_settings.wifi_ssid,
                        wifi_mgr_ip().c_str());
  lv_label_set_text_fmt(s_inf_line[3], "MQTT      : %s:%u  %s", g_settings.mqtt_host,
                        (unsigned)g_settings.mqtt_port, mst);
  lv_label_set_text_fmt(s_inf_line[4], "MAC       : %s", WiFi.macAddress().c_str());
  lv_label_set_text_fmt(s_inf_line[5], "Flash     : %u KB   RAM bebas: %u KB",
                        (unsigned)(ESP.getFlashChipSize() / 1024),
                        (unsigned)(ESP.getFreeHeap() / 1024));
  lv_label_set_text_fmt(s_inf_line[6], "Uptime    : %s", up);
  snprintf(line, sizeof line, "Panel     : %s %ux%u", "ST7796", LCD_WIDTH, LCD_HEIGHT);
  lv_label_set_text(s_inf_line[7], line);
  lv_label_set_text_fmt(s_inf_line[8], "Registry  : id %u  %s", (unsigned)g_settings.floor_id,
                        registry_ready() ? registry_status() : "memuat...");
}

void ui_set_cal_hook(ui_cal_hook_t fn) { s_cal_hook = fn; }

void ui_next_page(void) { show_page((s_page + 1) % 3); }

static void setup_view(lv_obj_t *root) {
  lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(root, 0, 0);
  lv_obj_set_style_pad_all(root, 0, 0);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
}

void ui_init(void) {
  lv_theme_default_init(lv_disp_get_default(), lv_color_hex(COL_ACCENT),
                        lv_color_hex(COL_GREEN), true, LV_FONT_DEFAULT);

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(scr, 6, 0);
  lv_obj_set_style_pad_row(scr, 6, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  s_views[0] = lv_obj_create(scr);
  setup_view(s_views[0]);
  mk_dashboard(s_views[0]);

  s_views[1] = lv_obj_create(scr);
  setup_view(s_views[1]);
  mk_detail(s_views[1]);

  s_views[2] = lv_obj_create(scr);
  setup_view(s_views[2]);
  mk_info(s_views[2]);

  show_page(0);
  ui_refresh();
}

void ui_refresh(void) {
  /* rebuild room tabs when the registry (room list) changes or a tab was tap */
  if (s_tabs_dirty || registry_room_count() != s_tab_roomcount) {
    s_tabs_dirty = false;
    s_tab_roomcount = registry_room_count();
    build_tabs();
  }

  /* top bar */
  if (wifi_mgr_portal_active()) {
    lv_label_set_text(s_lbl_net, "SETUP AP");
    lv_obj_set_style_text_color(s_lbl_net, lv_color_hex(COL_AMBER), 0);
  } else {
    const char *w = "WiFi --";
    if (wifi_mgr_state() == WIFI_ST_OK) w = "WiFi OK";
    else if (wifi_mgr_state() == WIFI_ST_CONNECTING) w = "WiFi...";
    const char *m = mqtt_mgr_connected() ? "MQTT OK" : "MQTT --";
    lv_label_set_text_fmt(s_lbl_net, "%s  %s", w, m);
    lv_obj_set_style_text_color(s_lbl_net,
                                lv_color_hex(mqtt_mgr_connected() ? COL_GREEN : COL_MUTED), 0);
  }
  lv_label_set_text(s_lbl_floor, g_settings.floor_label);

  update_rows();

  /* environment */
  MonEnv e;
  monitor_env(&e);
  lv_label_set_text_fmt(s_lbl_env[0], "Suhu   %.1fC", e.die_temp_c);
  lv_label_set_text_fmt(s_lbl_env[1], "Cahaya %d%%", e.light_pct);
  lv_label_set_text_fmt(s_lbl_env[2], "Sinyal %d dBm", e.rssi);
  lv_label_set_text_fmt(s_lbl_env[3], "Memori %luk", (unsigned long)(e.heap_free / 1024));

  /* network status card */
  lv_label_set_text_fmt(s_lbl_net2[0], "WiFi  %s", wifi_mgr_state() == WIFI_ST_OK ? "OK" : "off");
  lv_label_set_text_fmt(s_lbl_net2[1], "MQTT  %s", mqtt_mgr_connected() ? "OK" : "--");
  lv_label_set_text_fmt(s_lbl_net2[2], "IP    %s", wifi_mgr_ip().c_str());

  /* bottom bar */
  lv_label_set_text_fmt(s_lbl_summary, "%d online / %d perangkat", monitor_online(),
                        monitor_count());
  char up[24];
  format_uptime(up, sizeof up);
  lv_label_set_text_fmt(s_lbl_uptime, "up %s", up);

  if (s_page == 1) update_detail();
  else if (s_page == 2) update_info();
}

