#include <Arduino.h>
#include <lvgl.h>

#include "board_pins.h"
#include "config.h"
#include "settings.h"
#include "registry.h"
#include "mqtt_mgr.h"
#include "ui.h"

#define COL_BG     0x0B1220
#define COL_CARD   0x111A2E
#define COL_CARD2  0x0E1626
#define COL_BORDER 0x1E2A44
#define COL_TEXT   0xE2E8F0
#define COL_MUTED  0x7A8AA0
#define COL_DIM    0x4A5A70
#define COL_ACCENT 0x3B82F6
#define COL_GREEN  0x22C55E
#define COL_AMBER  0xF5B400
#define COL_RED    0xEF4444

static lv_obj_t *s_main = NULL;
static lv_obj_t *s_set = NULL;

/* main-screen widgets */
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_status = NULL;
static lv_obj_t *s_btn[GEN_SLOTS];
static lv_obj_t *s_icon_box[GEN_SLOTS];
static lv_obj_t *s_icon_circle[GEN_SLOTS];
static lv_obj_t *s_icon_glow[GEN_SLOTS];
static lv_obj_t *s_icon_base[GEN_SLOTS]; /* rect (general) or line (spot) */
static lv_obj_t *s_name[GEN_SLOTS];
static lv_obj_t *s_state[GEN_SLOTS];

/* settings widgets */
static lv_obj_t *s_ta_ssid = NULL;
static lv_obj_t *s_ta_pass = NULL;
static lv_obj_t *s_ta_floor = NULL;
static lv_obj_t *s_ta_room = NULL;
static lv_obj_t *s_lbl_wifi = NULL;
static lv_obj_t *s_dd_dev[GEN_SLOTS];
static lv_obj_t *s_dd_icon[GEN_SLOTS];
static lv_obj_t *s_kb = NULL;
static lv_obj_t *s_set_content = NULL;

/* ------------------------------------------------------------------------- */
static bool str_eq_ci(const char *a, const char *b) {
  while (*a && *b) {
    if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                          uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  return l;
}

static void mk_ta(lv_obj_t *parent, lv_obj_t **out, const char *txt, bool password) {
  lv_obj_t *ta = lv_textarea_create(parent);
  lv_obj_set_size(ta, LV_PCT(100), 30);
  lv_textarea_set_one_line(ta, true);
  if (password) lv_textarea_set_password_mode(ta, true);
  lv_textarea_set_text(ta, txt ? txt : "");
  lv_obj_set_style_bg_color(ta, lv_color_hex(COL_CARD2), 0);
  lv_obj_set_style_text_color(ta, lv_color_hex(COL_TEXT), 0);
  lv_obj_set_style_text_font(ta, &lv_font_montserrat_12, 0);
  lv_obj_set_style_border_color(ta, lv_color_hex(COL_BORDER), 0);
  lv_obj_set_style_radius(ta, 8, 0);
  *out = ta;
}

/* ------------------------------------------------------------------------- */
/*  Lamp icons (bulb body + base or beam)                                    */
/* ------------------------------------------------------------------------- */
static void mk_icon(int i) {
  lv_obj_t *btn = s_btn[i];
  lv_obj_t *box = lv_obj_create(btn);
  lv_obj_set_size(box, 48, 44);
  lv_obj_set_pos(box, 31, 3);
  lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  s_icon_box[i] = box;

  lv_obj_t *circle = lv_obj_create(box);
  lv_obj_set_style_bg_color(circle, lv_color_hex(0x3E4C63), 0);
  lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(circle, 2, 0);
  lv_obj_set_style_border_color(circle, lv_color_hex(0x5A6B85), 0);
  s_icon_circle[i] = circle;

  /* bright highlight inside the bulb (glow when ON) */
  lv_obj_t *glow = lv_obj_create(box);
  lv_obj_set_size(glow, 10, 10);
  lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(glow, lv_color_hex(0xFFF3C1), 0);
  lv_obj_set_style_bg_opa(glow, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(glow, 0, 0);
  lv_obj_add_flag(glow, LV_OBJ_FLAG_HIDDEN);
  s_icon_glow[i] = glow;

  if (g_settings.slots[i].icon == 0) {
    /* spot lamp: head + bright expanding beam */
    lv_obj_set_size(circle, 24, 24);
    lv_obj_set_pos(circle, 12, 0);
    lv_obj_set_pos(glow, 19, 7);
    static lv_point_t beam_pts[5] = {{17, 25}, {2, 42}, {46, 42}, {31, 25}, {17, 25}};
    lv_obj_t *line = lv_line_create(box);
    lv_line_set_points(line, beam_pts, 5);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(0x3E4C63), 0);
    lv_obj_set_style_line_width(line, 3, 0);
    s_icon_base[i] = line;
  } else {
    /* general lamp: bulb + stem */
    lv_obj_set_size(circle, 28, 28);
    lv_obj_set_pos(circle, 10, -1);
    lv_obj_set_pos(glow, 18, 6);
    lv_obj_t *base = lv_obj_create(box);
    lv_obj_set_size(base, 16, 7);
    lv_obj_set_pos(base, 16, 29);
    lv_obj_set_style_bg_color(base, lv_color_hex(0x3E4C63), 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(base, 0, 0);
    lv_obj_set_style_radius(base, 3, 0);
    s_icon_base[i] = base;
  }
}

static void icon_apply_state(int i, bool known, bool on) {
  uint32_t col = !known ? 0x3E4C63 : (on ? 0xFFC107 : 0x4A5C77);
  uint32_t ring = !known ? 0x5A6B85 : (on ? 0xFFE082 : 0x5A6B85);
  lv_obj_set_style_bg_color(s_icon_circle[i], lv_color_hex(col), 0);
  lv_obj_set_style_border_color(s_icon_circle[i], lv_color_hex(ring), 0);
  if (g_settings.slots[i].icon == 0)
    lv_obj_set_style_line_color(s_icon_base[i], lv_color_hex(col), 0);
  else
    lv_obj_set_style_bg_color(s_icon_base[i], lv_color_hex(col), 0);
  if (on) lv_obj_clear_flag(s_icon_glow[i], LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(s_icon_glow[i], LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------------- */
/*  Main screen                                                              */
/* ------------------------------------------------------------------------- */
static void refresh_title(void) {
  char buf[64];
  snprintf(buf, sizeof buf, "%s | %s", g_settings.floor_label, g_settings.room_label);
  lv_label_set_text(s_title, buf);
}

static void btn_click_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= GEN_SLOTS) return;
  LampSlot *slot = &g_settings.slots[idx];
  int ri = slot->guid[0] ? registry_find_guid(slot->guid) : -1;
  if (ri < 0 && slot->label[0]) ri = registry_find_short(slot->label);
  if (ri >= 0) snprintf(slot->guid, sizeof slot->guid, "%s", registry_guid_at(ri));
  if (ri < 0) {
    ui_open_settings(); /* unassigned -> settings */
    return;
  }
  bool on = registry_state_known(ri) ? registry_state_on(ri) : false;
  /* toggle: HIOTO active-low (0 = ON, 1 = OFF) */
  mqtt_publish_actuator(registry_guid_at(ri), on ? 1 : 0);
  registry_set_state(ri, !on);
  ui_refresh_states();
}

static void settings_click_cb(lv_event_t *e) {
  (void)e;
  ui_open_settings();
}

static void mk_main(void) {
  s_main = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s_main, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(s_main, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(s_main, 8, 0);
  lv_obj_set_style_pad_row(s_main, 6, 0);
  lv_obj_clear_flag(s_main, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(s_main, LV_FLEX_FLOW_COLUMN);

  /* top bar: title + status + settings pill */
  lv_obj_t *top = lv_obj_create(s_main);
  lv_obj_set_size(top, LV_PCT(100), 34);
  lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(top, 0, 0);
  lv_obj_set_style_pad_all(top, 0, 0);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(top, 8, 0);
  lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

  s_title = mk_label(top, "Lantai 1 \xC2\xB7 Living Room", &lv_font_montserrat_20, COL_TEXT);
  lv_obj_t *sp = lv_obj_create(top);
  lv_obj_set_size(sp, 6, 1);
  lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(sp, 0, 0);

  s_status = mk_label(top, "", &lv_font_montserrat_12, COL_MUTED);

  lv_obj_t *set = lv_btn_create(top);
  lv_obj_set_size(set, 96, 30);
  lv_obj_set_style_radius(set, 15, 0);
  lv_obj_set_style_bg_color(set, lv_color_hex(COL_CARD), 0);
  lv_obj_set_style_border_width(set, 1, 0);
  lv_obj_set_style_border_color(set, lv_color_hex(COL_BORDER), 0);
  lv_obj_add_event_cb(set, settings_click_cb, LV_EVENT_CLICKED, NULL);
  mk_label(set, "Setelan", &lv_font_montserrat_12, COL_TEXT);

  /* 2 rows x 4 buttons */
  for (int r = 0; r < 2; r++) {
    lv_obj_t *row = lv_obj_create(s_main);
    lv_obj_set_size(row, LV_PCT(100), 122);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for (int c = 0; c < 4; c++) {
      int i = r * 4 + c;
      lv_obj_t *btn = lv_btn_create(row);
      lv_obj_set_size(btn, 110, 116);
      lv_obj_set_style_radius(btn, 14, 0);
      lv_obj_set_style_bg_color(btn, lv_color_hex(COL_CARD), 0);
      lv_obj_set_style_border_width(btn, 1, 0);
      lv_obj_set_style_border_color(btn, lv_color_hex(COL_BORDER), 0);
      lv_obj_add_event_cb(btn, btn_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
      s_btn[i] = btn;

      mk_icon(i);
      s_name[i] = mk_label(btn, g_settings.slots[i].label, &lv_font_montserrat_14, COL_TEXT);
      lv_obj_set_pos(s_name[i], 8, 48);
      s_state[i] = mk_label(btn, "-", &lv_font_montserrat_14, COL_DIM);
      lv_obj_set_pos(s_state[i], 8, 70);
    }
  }
}

/* ------------------------------------------------------------------------- */
/*  Settings screen                                                          */
/* ------------------------------------------------------------------------- */
static void ta_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (!s_kb) return;
  if (code == LV_EVENT_FOCUSED) {
    lv_keyboard_set_textarea(s_kb, lv_event_get_target(e));
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  }
}

static void back_click_cb(lv_event_t *e) {
  (void)e;
  lv_scr_load(s_main);
}

static void rebuild_icon(int i) {
  if (s_icon_box[i]) lv_obj_del(s_icon_box[i]);
  mk_icon(i);
  lv_obj_set_pos(s_name[i], 8, 48);
  lv_obj_set_pos(s_state[i], 8, 70);
}

static void save_click_cb(lv_event_t *e) {
  (void)e;
  if (s_ta_ssid) snprintf(g_settings.wifi_ssid, sizeof g_settings.wifi_ssid, "%s",
                          lv_textarea_get_text(s_ta_ssid));
  if (s_ta_pass) snprintf(g_settings.wifi_pass, sizeof g_settings.wifi_pass, "%s",
                          lv_textarea_get_text(s_ta_pass));
  if (s_ta_floor) snprintf(g_settings.floor_label, sizeof g_settings.floor_label, "%s",
                           lv_textarea_get_text(s_ta_floor));
  if (s_ta_room) snprintf(g_settings.room_label, sizeof g_settings.room_label, "%s",
                          lv_textarea_get_text(s_ta_room));
  for (int i = 0; i < GEN_SLOTS; i++) {
    if (!s_dd_dev[i]) continue;
    uint16_t sel = lv_dropdown_get_selected(s_dd_dev[i]);
    int ri = (int)sel - 1; /* option 0 is "(kosong)" */
    if (ri >= 0 && ri < registry_count()) {
      snprintf(g_settings.slots[i].guid, sizeof g_settings.slots[i].guid, "%s",
               registry_guid_at(ri));
      snprintf(g_settings.slots[i].label, sizeof g_settings.slots[i].label, "%s",
               registry_shortname(ri));
    }
    if (s_dd_icon[i]) g_settings.slots[i].icon = (uint8_t)lv_dropdown_get_selected(s_dd_icon[i]);
  }
  settings_save(&g_settings);
  wifi_apply(g_settings.wifi_ssid, g_settings.wifi_pass);
  refresh_title();
  for (int i = 0; i < GEN_SLOTS; i++) {
    lv_label_set_text(s_name[i], g_settings.slots[i].label);
    rebuild_icon(i);
  }
  lv_scr_load(s_main);
  ui_refresh_states();
}

static void mk_settings(void) {
  s_set = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s_set, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(s_set, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_set, LV_OBJ_FLAG_SCROLLABLE);

  /* scrollable content */
  s_set_content = lv_obj_create(s_set);
  lv_obj_set_size(s_set_content, LCD_WIDTH, LCD_HEIGHT - 56);
  lv_obj_set_pos(s_set_content, 0, 0);
  lv_obj_set_style_bg_opa(s_set_content, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_set_content, 0, 0);
  lv_obj_set_style_pad_all(s_set_content, 8, 0);
  lv_obj_set_style_pad_row(s_set_content, 6, 0);
  lv_obj_set_flex_flow(s_set_content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(s_set_content, LV_DIR_VER);

  mk_label(s_set_content, "SETELAN", &lv_font_montserrat_20, COL_TEXT);

  /* ---- WiFi ---- */
  mk_label(s_set_content, "WiFi", &lv_font_montserrat_14, COL_ACCENT);
  mk_ta(s_set_content, &s_ta_ssid, g_settings.wifi_ssid, false);
  mk_ta(s_set_content, &s_ta_pass, g_settings.wifi_pass, true);
  lv_obj_add_event_cb(s_ta_ssid, ta_event_cb, LV_EVENT_FOCUSED, NULL);
  lv_obj_add_event_cb(s_ta_ssid, ta_event_cb, LV_EVENT_DEFOCUSED, NULL);
  lv_obj_add_event_cb(s_ta_pass, ta_event_cb, LV_EVENT_FOCUSED, NULL);
  lv_obj_add_event_cb(s_ta_pass, ta_event_cb, LV_EVENT_DEFOCUSED, NULL);
  s_lbl_wifi = mk_label(s_set_content, wifi_state_text(), &lv_font_montserrat_12, COL_MUTED);

  /* ---- title fields ---- */
  mk_label(s_set_content, "Tampilan (judul)", &lv_font_montserrat_14, COL_ACCENT);
  lv_obj_t *row1 = lv_obj_create(s_set_content);
  lv_obj_set_size(row1, LV_PCT(100), 34);
  lv_obj_set_style_bg_opa(row1, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row1, 0, 0);
  lv_obj_set_style_pad_all(row1, 0, 0);
  lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row1, 6, 0);
  lv_obj_clear_flag(row1, LV_OBJ_FLAG_SCROLLABLE);
  mk_ta(row1, &s_ta_floor, g_settings.floor_label, false);
  mk_ta(row1, &s_ta_room, g_settings.room_label, false);
  lv_obj_set_width(s_ta_floor, LV_PCT(45));
  lv_obj_set_width(s_ta_room, LV_PCT(45));
  lv_obj_add_event_cb(s_ta_floor, ta_event_cb, LV_EVENT_FOCUSED, NULL);
  lv_obj_add_event_cb(s_ta_floor, ta_event_cb, LV_EVENT_DEFOCUSED, NULL);
  lv_obj_add_event_cb(s_ta_room, ta_event_cb, LV_EVENT_FOCUSED, NULL);
  lv_obj_add_event_cb(s_ta_room, ta_event_cb, LV_EVENT_DEFOCUSED, NULL);

  /* ---- slot assignment ---- */
  mk_label(s_set_content, "Tombol lampu (lampu + ikon)", &lv_font_montserrat_14, COL_ACCENT);
  for (int i = 0; i < GEN_SLOTS; i++) {
    lv_obj_t *row = lv_obj_create(s_set_content);
    lv_obj_set_size(row, LV_PCT(100), 34);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    char num[8];
    snprintf(num, sizeof num, "%d", i + 1);
    mk_label(row, num, &lv_font_montserrat_14, COL_MUTED);

    lv_obj_t *dd1 = lv_dropdown_create(row);
    lv_obj_set_width(dd1, 190);
    lv_obj_set_height(dd1, 30);
    lv_obj_set_style_text_font(dd1, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(dd1, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_text_color(dd1, lv_color_hex(COL_TEXT), 0);
    s_dd_dev[i] = dd1;

    lv_obj_t *dd2 = lv_dropdown_create(row);
    lv_obj_set_width(dd2, 96);
    lv_obj_set_height(dd2, 30);
    lv_obj_set_style_text_font(dd2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(dd2, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_text_color(dd2, lv_color_hex(COL_TEXT), 0);
    lv_dropdown_set_options(dd2, "Spot\nUmum");
    lv_dropdown_set_selected(dd2, g_settings.slots[i].icon ? 1 : 0);
    s_dd_icon[i] = dd2;
  }

  /* ---- bottom bar (safe interior zone) ---- */
  lv_obj_t *bar = lv_obj_create(s_set);
  lv_obj_set_size(bar, LV_PCT(100), 48);
  lv_obj_set_pos(bar, 0, LCD_HEIGHT - 48);
  lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 4, 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *back = lv_btn_create(bar);
  lv_obj_set_size(back, 150, 38);
  lv_obj_set_style_bg_color(back, lv_color_hex(COL_CARD), 0);
  lv_obj_add_event_cb(back, back_click_cb, LV_EVENT_CLICKED, NULL);
  mk_label(back, "< Kembali", &lv_font_montserrat_14, COL_TEXT);

  lv_obj_t *save = lv_btn_create(bar);
  lv_obj_set_size(save, 150, 38);
  lv_obj_set_style_bg_color(save, lv_color_hex(COL_ACCENT), 0);
  lv_obj_add_event_cb(save, save_click_cb, LV_EVENT_CLICKED, NULL);
  mk_label(save, "Simpan", &lv_font_montserrat_14, COL_TEXT);

  /* keyboard (hidden until a textarea is focused) */
  s_kb = lv_keyboard_create(s_set);
  lv_obj_set_size(s_kb, LV_PCT(100), 160);
  lv_obj_set_pos(s_kb, 0, LCD_HEIGHT - 160);
  lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------------- */
static void rebuild_dropdowns(void) {
  for (int i = 0; i < GEN_SLOTS; i++) {
    if (!s_dd_dev[i]) continue;
    char opts[700];
    strcpy(opts, "(kosong)");
    int sel = 0;
    const char *want = g_settings.slots[i].guid;
    const char *want_label = g_settings.slots[i].label;
    for (int k = 0; k < registry_count(); k++) {
      const char *sn = registry_shortname(k);
      if (strlen(opts) + strlen(sn) + 2 >= sizeof opts) break;
      strcat(opts, "\n");
      strcat(opts, sn);
      if (want[0] && !strcmp(registry_guid_at(k), want)) sel = k + 1;
      else if (!want[0] && want_label[0] && str_eq_ci(sn, want_label)) sel = k + 1;
    }
    lv_dropdown_set_options(s_dd_dev[i], opts);
    lv_dropdown_set_selected(s_dd_dev[i], sel);
  }
}

void ui_registry_changed(void) { rebuild_dropdowns(); }

void ui_open_settings(void) {
  rebuild_dropdowns();
  if (s_lbl_wifi) lv_label_set_text(s_lbl_wifi, wifi_state_text());
  lv_scr_load(s_set);
}

/* ------------------------------------------------------------------------- */
void ui_refresh_states(void) {
  static uint32_t last = 0;
  if (millis() - last < 250) return;
  last = millis();

  char st[96];
  snprintf(st, sizeof st, "%s  %s", wifi_state_text(),
           mqtt_connected() ? "MQTT OK" : "MQTT --");
  if (s_status) {
    lv_label_set_text(s_status, st);
    lv_obj_set_style_text_color(s_status,
                                lv_color_hex(mqtt_connected() ? COL_GREEN : COL_MUTED), 0);
  }
  if (s_lbl_wifi) lv_label_set_text(s_lbl_wifi, wifi_state_text());

  for (int i = 0; i < GEN_SLOTS; i++) {
    if (!s_btn[i]) continue;
    LampSlot *slot = &g_settings.slots[i];
    int ri = slot->guid[0] ? registry_find_guid(slot->guid) : registry_find_short(slot->label);
    bool known = ri >= 0 && registry_state_known(ri);
    bool on = known && registry_state_on(ri);
    icon_apply_state(i, known, on);

    lv_label_set_text(s_name[i], slot->label[0] ? slot->label : "kosong");
    lv_obj_set_style_text_color(
        s_name[i], lv_color_hex(ri >= 0 ? COL_TEXT : COL_MUTED), 0);

    const char *stt = !known ? "-" : (on ? "ON" : "OFF");
    lv_label_set_text(s_state[i], stt);
    lv_obj_set_style_text_color(s_state[i],
                                lv_color_hex(on ? COL_AMBER : (!known ? COL_DIM : COL_MUTED)), 0);
    lv_obj_set_style_bg_color(s_btn[i], lv_color_hex(on ? 0x1E2E16 : COL_CARD), 0);
    lv_obj_set_style_border_color(s_btn[i], lv_color_hex(on ? COL_AMBER : COL_BORDER), 0);
  }
}

void ui_init(void) {
  lv_theme_default_init(lv_disp_get_default(), lv_color_hex(COL_ACCENT),
                        lv_color_hex(COL_GREEN), true, LV_FONT_DEFAULT);
  mk_main();
  mk_settings();
  refresh_title();
  lv_scr_load(s_main);
  ui_refresh_states();
}
