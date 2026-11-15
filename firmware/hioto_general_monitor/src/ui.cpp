#include <Arduino.h>
#include <lvgl.h>

#include "board_pins.h"
#include "config.h"
#include "settings.h"
#include "registry.h"
#include "catalog.h"
#include "mqtt_mgr.h"
#include "theme.h"
#include "backlight.h"
#include "ui.h"

/* Real panel size after rotation; set before ui_init(). */
static lv_coord_t UI_W = 480;
static lv_coord_t UI_H = 320;
/* Button grid comes from the build (config.h): 4x2 on the 480x320 3.5" panel,
 * 3x2 on the 320x240 2.8" panel. The panel size drives fonts and metrics only,
 * never the slot count, so the main grid and the settings slot rows cannot
 * disagree. */
static int UI_COLS = GEN_COLS;
static int UI_ROWS = GEN_ROWS;

void ui_set_screen(int w, int h) {
  if (w > 0) UI_W = (lv_coord_t)w;
  if (h > 0) UI_H = (lv_coord_t)h;

  /* Loud check for a mis-built env rather than a silent re-flow: a 320 px panel
   * cannot fit four comfortable buttons per row, and a grid whose cell count
   * differs from GEN_SLOTS would orphan configuration rows. */
  const int fit = (UI_W >= 400) ? 4 : 3;
  if (fit != UI_COLS) {
    Serial.printf("[ui] WARNING panel %dx%d fits %d columns but GEN_SLOTS=%d (%dx%d)\n",
                  (int)UI_W, (int)UI_H, fit, GEN_SLOTS, UI_COLS, UI_ROWS);
  }
}

/* Active palette. C(field) expands to the colour of the current theme. */
static inline const UiTheme *T(void) { return theme_get(g_settings.theme); }
#define C(field) lv_color_hex(T()->field)

/* ------------------------------------------------------------------------- */
/*  Widget handles                                                           */
/* ------------------------------------------------------------------------- */
static lv_obj_t *s_main = NULL;
static lv_obj_t *s_set = NULL;

/* main screen */
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_ic_wifi = NULL; /* glyph inside the wifi pill (tinted live) */
static lv_obj_t *s_ic_mqtt = NULL; /* glyph inside the mqtt pill */
static lv_obj_t *s_ic_set = NULL;  /* glyph inside the settings pill */
static lv_obj_t *s_btn[GEN_SLOTS];
static lv_obj_t *s_icon_box[GEN_SLOTS];
static lv_obj_t *s_icon_fill[GEN_SLOTS][4]; /* shapes painted in the lamp colour */
static lv_obj_t *s_icon_line[GEN_SLOTS][2]; /* beam / rays */
static lv_obj_t *s_icon_glow[GEN_SLOTS];    /* hot spot, visible when ON */
static lv_obj_t *s_name[GEN_SLOTS];
static lv_obj_t *s_state[GEN_SLOTS];

/* settings screen */
static lv_obj_t *s_ta_ssid = NULL;
static lv_obj_t *s_ta_pass = NULL;
static lv_obj_t *s_dd_floor = NULL;
static lv_obj_t *s_dd_room = NULL;
static lv_obj_t *s_lbl_wifi = NULL;
static lv_obj_t *s_dd_dev[GEN_SLOTS];
static lv_obj_t *s_dd_icon[GEN_SLOTS];
static lv_obj_t *s_lbl_alias[GEN_SLOTS]; /* friendly name shown in the row */

/* alias editor overlay */
static lv_obj_t *s_edit = NULL;
static lv_obj_t *s_edit_lbl = NULL;
static lv_obj_t *s_edit_ta = NULL;
static lv_obj_t *s_edit_kb = NULL;
static int s_edit_slot = -1;
static lv_obj_t *s_kb = NULL;
static lv_obj_t *s_set_content = NULL;
static lv_obj_t *s_slider = NULL;
static lv_obj_t *s_lbl_bright = NULL;
static lv_obj_t *s_dd_theme = NULL;
static lv_obj_t *s_dd_scope = NULL;
static lv_obj_t *s_lbl_scope = NULL;

/* Device dropdown options: index k of the dropdown shows registry entry
 * s_opt_reg[k]. Filtering shortens this list, so every dropdown needs the
 * map back to the registry. */
/* ------------------------------------------------------------------------- */
/*  Layout: one code base, two panels (480x320 3.5" and 320x240 2.8")         */
/* ------------------------------------------------------------------------- */
#define UI_BIG (UI_W >= 400)

#define F_TITLE (UI_BIG ? &lv_font_montserrat_20 : &lv_font_montserrat_14)
#define F_ROW (UI_BIG ? &lv_font_montserrat_14 : &lv_font_montserrat_12)
#define F_SMALL (&lv_font_montserrat_12)

#define TOP_H (UI_BIG ? 34 : 26)
/* wifi / mqtt / settings are round icon buttons instead of text, so the top
 * bar stays slim and the title keeps the width */
#define TOP_ICON (UI_BIG ? 30 : 24)
#define TOP_ICON_FONT (UI_BIG ? &lv_font_montserrat_20 : &lv_font_montserrat_16)

/* compact 320x240 budget: 8 pad + 26 top + 2*6 row gaps + 2*92 rows + 8 pad = 238 */
#define SLOT_ROW_H (UI_BIG ? 122 : 92)
#define BTN_W ((UI_W - 16 - (UI_COLS - 1) * 6) / UI_COLS)
#define UI_SLOTS (UI_COLS * UI_ROWS)
#define BTN_H (UI_BIG ? 116 : 88)
#define ICON_X ((BTN_W - 48) / 2)
#define NAME_Y (UI_BIG ? 48 : 46)
#define NAME_W (BTN_W - 12)
#define STATE_Y (UI_BIG ? 70 : 64)

#define SET_H (UI_H - (UI_BIG ? 56 : 52))
#define BAR_H (UI_BIG ? 48 : 40)
#define SET_ROW_H (UI_BIG ? 34 : 30)
#define DD_DEV_W (UI_BIG ? 158 : 104)
#define DD_ICON_W (UI_BIG ? 100 : 64)
#define ALIAS_W (UI_BIG ? 148 : 96)

/* On-screen keyboard heights. The LVGL default map needs 4 rows; these sizes
 * leave every row visible on each panel. */
#define KB_MAIN_H (UI_BIG ? 200 : 150)
#define KB_EDIT_H (UI_BIG ? 228 : 160)

#define EDIT_FIELD_Y (UI_BIG ? 18 : 16)
#define EDIT_FIELD_H (UI_BIG ? 34 : 30)
#define EDIT_BTN_Y (UI_BIG ? 56 : 50)
#define EDIT_BTN_H (UI_BIG ? 32 : 28)
#define DD_W (UI_BIG ? 190 : 150)
#define SLIDER_W (UI_BIG ? 190 : 110)
#define BSTEP_W (UI_BIG ? 40 : 32)
#define VAL_W (UI_BIG ? 48 : 44)
#define REG_OPT_MAX 160

/* Connection state of the top-bar icons. Deliberately NOT taken from the theme:
 * these two glyphs report whether Wi-Fi and the broker are up, so they must read
 * the same way in every palette. Borrowing the palette's "ok" colour made the
 * meaning drift - Senja's accent is orange, so a healthy link looked like a
 * fault. Green = up, red = down, in all six themes. */
#define LINK_UP_HEX 0x22C55E
#define LINK_DOWN_HEX 0xEF4444
static int s_opt_reg[REG_OPT_MAX];
static int s_opt_count = 0;

/* Last applied state per slot. Rewriting a label or colour invalidates the
 * object and repaints it, so refreshing four times a second repainted the whole
 * panel continuously even when nothing had changed - that is what made the UI
 * feel sluggish. Only touch the widgets when a value really differs. */
static char s_last_name[GEN_SLOTS][24];
static int8_t s_last_state[GEN_SLOTS]; /* -1 unknown, 0 off, 1 on */
static bool s_last_wifi = false, s_last_mqtt = false, s_last_icons = false;

static uint32_t s_last_refresh = 0;
static bool s_rebuild_pending = false;
static bool s_dd_dirty = false;
static bool s_cal_request = false;

static void rebuild_dropdowns(void);

/* Touch calibration: request it here, run it from ui_poll() (outside the
 * LVGL event) so the blocking routine cannot corrupt widget state. */
static void cal_btn_cb(lv_event_t *e) {
  (void)e;
  s_cal_request = true;
}

/* ------------------------------------------------------------------------- */
/*  Small helpers                                                            */
/* ------------------------------------------------------------------------- */
static bool ci_eq(const char *a, const char *b) {
  while (*a && *b) {
    if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

/* Friendly word for a registry token. */
static const char *word_alias(const char *tok) {
  static const char *const kKeys[] = {"LAMP",      "SMARTSTEKER", "SMARTPLUG",
                                      "PANICBUTTON", "TOMBOLDARURAT", "AKTUATOR",
                                      "SAKLAR",    "SIRINE",      "DOORLOCK"};
  static const char *const kVals[] = {"Lampu",      "Steker",    "Steker",
                                      "Panic Button", "Tombol Darurat", "Aktuator",
                                      "Saklar",    "Sirine",     "Door Lock"};
  for (size_t i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++)
    if (ci_eq(tok, kKeys[i])) return kVals[i];
  return NULL;
}

/* "LAMP.14" -> "Lampu 14", "SMARTSTEKER-16" -> "Steker 16",
 * "DO-270-01" -> "DO 270 01", "IWK-SAKLAR-79" -> "IWK Saklar 79". */
static void pretty_alias(const char *label, char *out, size_t n) {
  out[0] = '\0';
  if (!label || !label[0]) return;

  char buf[64];
  strncpy(buf, label, sizeof buf - 1);
  buf[sizeof buf - 1] = '\0';

  bool after_lamp = false;
  char *p = buf;
  while (*p) {
    char tok[24];
    size_t t = 0;
    while (*p && *p != '.' && *p != '-' && t + 1 < sizeof tok) tok[t++] = *p++;
    tok[t] = '\0';
    if (*p) p++; /* skip the separator */
    if (!tok[0]) continue;

    bool numeric = true;
    for (size_t k = 0; tok[k]; k++)
      if (!isdigit((unsigned char)tok[k])) {
        numeric = false;
        break;
      }

    char piece[28];
    const char *map = word_alias(tok);
    if (map) {
      snprintf(piece, sizeof piece, "%s", map);
      after_lamp = ci_eq(tok, "LAMP");
    } else if (numeric) {
      if (after_lamp)
        snprintf(piece, sizeof piece, "%ld", strtol(tok, NULL, 10)); /* 09 -> 9 */
      else
        snprintf(piece, sizeof piece, "%s", tok);
      after_lamp = false;
    } else {
      bool allcaps = true;
      for (size_t k = 0; tok[k]; k++)
        if (isalpha((unsigned char)tok[k]) && !isupper((unsigned char)tok[k])) allcaps = false;
      snprintf(piece, sizeof piece, "%s", tok);
      if (!(allcaps && strlen(tok) <= 3)) { /* keep DO / DI / IWK as-is */
        piece[0] = (char)toupper((unsigned char)piece[0]);
        for (size_t k = 1; piece[k]; k++) piece[k] = (char)tolower((unsigned char)piece[k]);
      }
      after_lamp = false;
    }

    if (out[0] && strlen(out) + strlen(piece) + 2 < n) strcat(out, " ");
    strncat(out, piece, n - strlen(out) - 1);
  }
}

/* Name shown on the panel button: the alias, else a friendly form of the id. */
static void slot_display_name(int i, char *out, size_t n) {
  const LampSlot *s = &g_settings.slots[i];
  if (s->alias[0]) {
    snprintf(out, n, "%s", s->alias);
    return;
  }
  if (s->label[0]) {
    pretty_alias(s->label, out, n);
    return;
  }
  snprintf(out, n, "kosong");
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                          lv_color_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  return l;
}

/* Bare container: transparent, no border, no padding, no scrolling. */
static lv_obj_t *mk_row(lv_obj_t *parent, lv_coord_t h) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_size(row, LV_PCT(100), h);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_style_pad_column(row, 6, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  return row;
}

static void mk_ta(lv_obj_t *parent, lv_obj_t **out, const char *txt, bool password) {
  lv_obj_t *ta = lv_textarea_create(parent);
  lv_obj_set_size(ta, LV_PCT(100), 30);
  lv_textarea_set_one_line(ta, true);
  if (password) lv_textarea_set_password_mode(ta, true);
  lv_textarea_set_text(ta, txt ? txt : "");
  lv_obj_set_style_bg_color(ta, C(card2), 0);
  lv_obj_set_style_text_color(ta, C(text), 0);
  lv_obj_set_style_text_font(ta, &lv_font_montserrat_12, 0);
  lv_obj_set_style_border_color(ta, C(border), 0);
  lv_obj_set_style_radius(ta, 8, 0);
  *out = ta;
}

static lv_obj_t *mk_shape(lv_obj_t *box, lv_coord_t w, lv_coord_t h, lv_coord_t x,
                          lv_coord_t y, lv_coord_t radius) {
  lv_obj_t *o = lv_obj_create(box);
  lv_obj_set_size(o, w, h);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_bg_color(o, C(icon_unk), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t *mk_bulb(lv_obj_t *box, lv_coord_t d, lv_coord_t x, lv_coord_t y) {
  lv_obj_t *o = mk_shape(box, d, d, x, y, LV_RADIUS_CIRCLE);
  lv_obj_set_style_border_width(o, 2, 0);
  lv_obj_set_style_border_color(o, C(icon_unk_ring), 0);
  return o;
}

static void mk_glow(lv_obj_t *box, int i, lv_coord_t w, lv_coord_t h, lv_coord_t x,
                    lv_coord_t y, lv_coord_t radius) {
  lv_obj_t *g = mk_shape(box, w, h, x, y, radius);
  lv_obj_set_style_bg_color(g, C(glow), 0);
  lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
  s_icon_glow[i] = g;
}

static void mk_line(lv_obj_t *box, int i, int slot, const lv_point_t *pts, uint16_t n) {
  lv_obj_t *ln = lv_line_create(box);
  lv_line_set_points(ln, pts, n);
  lv_obj_set_pos(ln, 0, 0);
  lv_obj_set_style_line_width(ln, 3, 0);
  lv_obj_set_style_line_rounded(ln, true, 0);
  lv_obj_set_style_line_color(ln, C(icon_unk), 0);
  s_icon_line[i][slot] = ln;
}

/* ------------------------------------------------------------------------- */
/*  Lamp icons                                                               */
/* ------------------------------------------------------------------------- */
static void mk_icon(int i) {
  lv_obj_t *box = lv_obj_create(s_btn[i]);
  lv_obj_set_size(box, 48, 44);
  lv_obj_set_pos(box, ICON_X, UI_BIG ? 3 : 2);
  lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  s_icon_box[i] = box;

  static lv_point_t spot_beam[5] = {{17, 25}, {2, 42}, {46, 42}, {31, 25}, {17, 25}};
  static lv_point_t ray_l[2] = {{14, 24}, {7, 41}};
  static lv_point_t ray_r[2] = {{34, 24}, {41, 41}};

  switch (g_settings.slots[i].icon % ICON_COUNT) {
    case 0: /* Spot — lamp head with a bright expanding beam */
      s_icon_fill[i][0] = mk_bulb(box, 24, 12, 0);
      mk_line(box, i, 0, spot_beam, 5);
      mk_glow(box, i, 10, 10, 19, 7, LV_RADIUS_CIRCLE);
      break;

    case 1: /* Umum — classic bulb on a screw base */
      s_icon_fill[i][0] = mk_bulb(box, 28, 10, 0);
      s_icon_fill[i][1] = mk_shape(box, 16, 7, 16, 30, 3);
      mk_glow(box, i, 10, 10, 19, 8, LV_RADIUS_CIRCLE);
      break;

    case 2: /* TL — linear tube with two end caps */
      s_icon_fill[i][0] = mk_shape(box, 38, 12, 5, 8, 6);
      s_icon_fill[i][1] = mk_shape(box, 5, 18, 1, 5, 2);
      s_icon_fill[i][2] = mk_shape(box, 5, 18, 42, 5, 2);
      mk_glow(box, i, 26, 4, 11, 12, 2);
      break;

    case 3: /* Downlight — ceiling plate, dome and two light rays */
      s_icon_fill[i][0] = mk_shape(box, 38, 6, 5, 0, 3);
      s_icon_fill[i][1] = mk_shape(box, 24, 15, 12, 6, 12);
      mk_line(box, i, 0, ray_l, 2);
      mk_line(box, i, 1, ray_r, 2);
      mk_glow(box, i, 18, 6, 15, 12, 3);
      break;

    default: /* Taman — post lamp: head, pole, foot */
      s_icon_fill[i][0] = mk_bulb(box, 20, 14, 2);
      s_icon_fill[i][1] = mk_shape(box, 5, 14, 21, 24, 2);
      s_icon_fill[i][2] = mk_shape(box, 20, 5, 14, 38, 2);
      mk_glow(box, i, 8, 8, 20, 8, LV_RADIUS_CIRCLE);
      break;
  }
}

static void icon_apply_state(int i, bool known, bool on) {
  const uint32_t fill = !known ? T()->icon_unk : (on ? T()->icon_on : T()->icon_off);
  const uint32_t ring =
      !known ? T()->icon_unk_ring : (on ? T()->icon_on_ring : T()->icon_off_ring);
  const lv_color_t cfill = lv_color_hex(fill);
  const lv_color_t cring = lv_color_hex(ring);

  for (int k = 0; k < 4; k++) {
    lv_obj_t *o = s_icon_fill[i][k];
    if (!o) continue;
    lv_obj_set_style_bg_color(o, cfill, 0);
    lv_obj_set_style_border_color(o, cring, 0);
  }
  for (int k = 0; k < 2; k++) {
    lv_obj_t *ln = s_icon_line[i][k];
    if (ln) lv_obj_set_style_line_color(ln, cfill, 0);
  }
  if (s_icon_glow[i]) {
    if (on) lv_obj_clear_flag(s_icon_glow[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_icon_glow[i], LV_OBJ_FLAG_HIDDEN);
  }
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
  int ri = registry_resolve(slot->guid, slot->label);
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

/* Round icon button for the top bar. Returns the glyph so the caller can
 * recolour it as the connection state changes. */
static lv_obj_t *mk_top_icon(lv_obj_t *parent, const char *sym) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, TOP_ICON, TOP_ICON);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(b, C(card), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, C(border), 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  /* resistive panel: widen the touch target beyond the visual circle */
  lv_obj_set_ext_click_area(b, UI_BIG ? 8 : 6);
  lv_obj_add_event_cb(b, settings_click_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = mk_label(b, sym, TOP_ICON_FONT, C(dim));
  lv_obj_center(l);
  return l;
}

static void mk_main(void) {
  s_main = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s_main, C(bg), 0);
  lv_obj_set_style_bg_opa(s_main, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(s_main, 8, 0);
  lv_obj_set_style_pad_row(s_main, 6, 0);
  lv_obj_clear_flag(s_main, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(s_main, LV_FLEX_FLOW_COLUMN);

  /* top bar: title (grows) + wifi + mqtt + settings icons */
  lv_obj_t *top = mk_row(s_main, TOP_H);
  lv_obj_set_style_pad_column(top, UI_BIG ? 8 : 6, 0);

  s_title = mk_label(top, "Lantai 1 | Living Room", F_TITLE, C(text));
  lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
  lv_obj_set_flex_grow(s_title, 1); /* take the leftover width, never push the icons */

  s_ic_wifi = mk_top_icon(top, LV_SYMBOL_WIFI);
  s_ic_mqtt = mk_top_icon(top, LV_SYMBOL_UPLOAD);
  s_ic_set = mk_top_icon(top, LV_SYMBOL_SETTINGS);

  /* UI_ROWS x UI_COLS buttons (4x2 or 3x2) */
  for (int r = 0; r < UI_ROWS; r++) {
    lv_obj_t *row = mk_row(s_main, SLOT_ROW_H);

    for (int c = 0; c < UI_COLS; c++) {
      int i = r * UI_COLS + c;
      lv_obj_t *btn = lv_btn_create(row);
      lv_obj_set_size(btn, BTN_W, BTN_H);
      lv_obj_set_style_radius(btn, 14, 0);
      lv_obj_set_style_bg_color(btn, C(card), 0);
      lv_obj_set_style_border_width(btn, 1, 0);
      lv_obj_set_style_border_color(btn, C(border), 0);
      lv_obj_add_event_cb(btn, btn_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
      s_btn[i] = btn;

      mk_icon(i);
      char nm[24];
      slot_display_name(i, nm, sizeof nm);
      s_name[i] = mk_label(btn, nm, F_ROW, C(text));
      lv_obj_set_pos(s_name[i], 6, NAME_Y);
      lv_obj_set_width(s_name[i], NAME_W);
      lv_label_set_long_mode(s_name[i], LV_LABEL_LONG_DOT);
      s_state[i] = mk_label(btn, "-", F_ROW, C(dim));
      lv_obj_set_pos(s_state[i], 6, STATE_Y + (UI_BIG ? 0 : 2));
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
    /* shrink the scroll area to the space above the keyboard and bring the
     * field into view, otherwise the keyboard covers what is being edited */
    if (s_set_content) {
      lv_obj_set_height(s_set_content, UI_H - KB_MAIN_H);
      lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
    }
  } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    if (s_set_content) lv_obj_set_height(s_set_content, SET_H);
  }
}

static void back_click_cb(lv_event_t *e) {
  (void)e;
  lv_scr_load(s_main);
}

/* ------------------------------------------------------------------------- */
/*  Title dropdowns: floor + room from the controller catalog                */
/* ------------------------------------------------------------------------- */
static void rebuild_location_dd(void) {
  if (s_dd_floor) {
    char buf[320];
    const int n = catalog_floor_count();
    if (n > 0) {
      buf[0] = '\0';
      for (int i = 0; i < n; i++) {
        if (i) strncat(buf, "\n", sizeof buf - strlen(buf) - 1);
        strncat(buf, catalog_floor_name(i), sizeof buf - strlen(buf) - 1);
      }
      lv_dropdown_set_options(s_dd_floor, buf);
      const int sel = catalog_find_floor(g_settings.floor_id, g_settings.floor_label);
      lv_dropdown_set_selected(s_dd_floor, sel >= 0 ? sel : 0);
    } else {
      lv_dropdown_set_options(s_dd_floor,
                              g_settings.floor_label[0] ? g_settings.floor_label : "-");
      lv_dropdown_set_selected(s_dd_floor, 0);
    }
  }

  if (s_dd_room) {
    char buf[1024];
    strcpy(buf, "(semua ruang)");
    const int n = catalog_room_count(g_settings.floor_id);
    for (int i = 0; i < n; i++) {
      const char *nm = catalog_room_name(g_settings.floor_id, i);
      if (strlen(buf) + strlen(nm) + 2 >= sizeof buf) break;
      strcat(buf, "\n");
      strcat(buf, nm);
    }
    if (n == 0 && g_settings.room_label[0]) { /* catalog down: keep the saved name */
      strcat(buf, "\n");
      strncat(buf, g_settings.room_label, sizeof buf - strlen(buf) - 1);
    }
    lv_dropdown_set_options(s_dd_room, buf);
    int sel = catalog_find_room(g_settings.floor_id, g_settings.room_id, g_settings.room_label);
    if (sel < 0 && n == 0 && g_settings.room_label[0]) sel = 0;
    lv_dropdown_set_selected(s_dd_room, sel >= 0 ? sel + 1 : 0);
  }
}

static void floor_dd_cb(lv_event_t *e) {
  const int idx = (int)lv_dropdown_get_selected(lv_event_get_target(e));
  if (idx >= 0 && idx < catalog_floor_count()) {
    g_settings.floor_id = catalog_floor_id(idx);
    snprintf(g_settings.floor_label, sizeof g_settings.floor_label, "%s",
             catalog_floor_name(idx));
  }
  /* keep the room when it belongs to the new floor, otherwise take the first */
  int ri = catalog_find_room(g_settings.floor_id, g_settings.room_id, g_settings.room_label);
  if (ri < 0) {
    if (catalog_room_count(g_settings.floor_id) > 0) {
      g_settings.room_id = catalog_room_id(g_settings.floor_id, 0);
      snprintf(g_settings.room_label, sizeof g_settings.room_label, "%s",
               catalog_room_name(g_settings.floor_id, 0));
    } else {
      g_settings.room_id = 0;
      g_settings.room_label[0] = '\0';
    }
  }
  settings_save(&g_settings);
  rebuild_location_dd(); /* room list follows the new floor (other widget) */
  refresh_title();
  s_dd_dirty = true; /* re-filter the device pickers outside this event */
}

static void room_dd_cb(lv_event_t *e) {
  const int idx = (int)lv_dropdown_get_selected(lv_event_get_target(e));
  if (idx == 0) { /* "(semua ruang)" */
    g_settings.room_id = 0;
    g_settings.room_label[0] = '\0';
  } else if (idx - 1 < catalog_room_count(g_settings.floor_id)) {
    g_settings.room_id = catalog_room_id(g_settings.floor_id, idx - 1);
    snprintf(g_settings.room_label, sizeof g_settings.room_label, "%s",
             catalog_room_name(g_settings.floor_id, idx - 1));
  }
  settings_save(&g_settings);
  refresh_title();
  s_dd_dirty = true;
}

static void refresh_slot_visual(int i) {
  if (s_icon_box[i]) {
    lv_obj_del(s_icon_box[i]);
    s_icon_box[i] = NULL;
    for (int k = 0; k < 4; k++) s_icon_fill[i][k] = NULL;
    for (int k = 0; k < 2; k++) s_icon_line[i][k] = NULL;
    s_icon_glow[i] = NULL;
  }
  mk_icon(i);
  lv_obj_set_pos(s_name[i], 8, 48);
  lv_obj_set_pos(s_state[i], 8, 70);
}

/* ---- brightness ---- */
static void bright_apply(int v, bool persist) {
  if (v < 20) v = 20;
  if (v > 100) v = 100;
  backlight_set((uint8_t)v);
  g_settings.brightness = (uint8_t)v;
  if (s_lbl_bright) {
    char b[16];
    snprintf(b, sizeof b, "%d%%", v);
    lv_label_set_text(s_lbl_bright, b);
  }
  if (persist) settings_save(&g_settings);
}

static void bright_slider_cb(lv_event_t *e) {
  int v = (int)lv_slider_get_value(lv_event_get_target(e));
  bright_apply(v, lv_event_get_code(e) == LV_EVENT_RELEASED);
}

static void bright_step_cb(lv_event_t *e) {
  int delta = (int)(intptr_t)lv_event_get_user_data(e);
  int v = (s_slider ? (int)lv_slider_get_value(s_slider) : (int)g_settings.brightness) + delta;
  if (v < 20) v = 20;
  if (v > 100) v = 100;
  if (s_slider) lv_slider_set_value(s_slider, v, LV_ANIM_OFF);
  bright_apply(v, true);
}

/* Picking another device drops the old friendly name so the new device's
 * automatic name shows up. */
static void dev_dd_cb(lv_event_t *e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= GEN_SLOTS) return;
  const int sel = (int)lv_dropdown_get_selected(lv_event_get_target(e));
  g_settings.slots[i].alias[0] = '\0';
  if (s_lbl_alias[i]) {
    char nm[24];
    if (sel > 0 && sel - 1 < s_opt_count)
      pretty_alias(registry_shortname(s_opt_reg[sel - 1]), nm, sizeof nm);
    else
      snprintf(nm, sizeof nm, "kosong");
    lv_label_set_text(s_lbl_alias[i], nm);
  }
}

/* ---- alias editor overlay ---- */
static void refresh_alias_labels(void) {
  for (int i = 0; i < GEN_SLOTS; i++) {
    if (!s_lbl_alias[i]) continue;
    char nm[24];
    slot_display_name(i, nm, sizeof nm);
    lv_label_set_text(s_lbl_alias[i], nm);
  }
}

static void close_alias_editor(bool apply, bool clear) {
  const int i = s_edit_slot;
  if (i >= 0 && i < GEN_SLOTS && (apply || clear)) {
    if (clear) {
      g_settings.slots[i].alias[0] = '\0';
    } else if (s_edit_ta) {
      snprintf(g_settings.slots[i].alias, sizeof g_settings.slots[i].alias, "%s",
               lv_textarea_get_text(s_edit_ta));
    }
    settings_save(&g_settings);
  }
  s_edit_slot = -1;
  if (s_edit) lv_obj_add_flag(s_edit, LV_OBJ_FLAG_HIDDEN);
  refresh_alias_labels();
}

static void alias_btn_cb(lv_event_t *e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (!s_edit || i < 0 || i >= GEN_SLOTS) return;
  s_edit_slot = i;
  if (s_edit_lbl) {
    char t[32];
    snprintf(t, sizeof t, "Nama tombol %d", i + 1);
    lv_label_set_text(s_edit_lbl, t);
  }
  char nm[24];
  slot_display_name(i, nm, sizeof nm);
  if (s_edit_ta) {
    lv_textarea_set_text(s_edit_ta, g_settings.slots[i].alias[0] ? g_settings.slots[i].alias : nm);
    lv_textarea_set_cursor_pos(s_edit_ta, LV_TEXTAREA_CURSOR_LAST);
  }
  if (s_edit_kb && s_edit_ta) lv_keyboard_set_textarea(s_edit_kb, s_edit_ta);
  if (s_kb) lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  if (s_set_content) lv_obj_set_height(s_set_content, SET_H);
  lv_obj_clear_flag(s_edit, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_edit);
}

static void edit_ok_cb(lv_event_t *e) {
  (void)e;
  close_alias_editor(true, false);
}
static void edit_cancel_cb(lv_event_t *e) {
  (void)e;
  close_alias_editor(false, false);
}
static void edit_clear_cb(lv_event_t *e) {
  (void)e;
  close_alias_editor(false, true);
}

/* Tapping a text field always shows the keyboard (focus events alone are not
 * reliable for a resistive panel). */
static void ta_click_cb(lv_event_t *e) {
  if (!s_kb) return;
  lv_keyboard_set_textarea(s_kb, lv_event_get_target(e));
  lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  if (s_set_content) {
    lv_obj_set_height(s_set_content, UI_H - KB_MAIN_H);
    lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
  }
}

/* ---- filter scope ---- */
static void scope_dd_cb(lv_event_t *e) {
  uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
  if (sel > 2) sel = 0;
  g_settings.scope = (uint8_t)sel;
  settings_save(&g_settings);
  s_dd_dirty = true; /* rebuild the option lists outside the dropdown event */
}

/* ---- theme ---- */
static void theme_dd_cb(lv_event_t *e) {
  uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
  if (sel >= THEME_COUNT) sel = 0;
  if (g_settings.theme == (uint8_t)sel) return;
  g_settings.theme = (uint8_t)sel;
  settings_save(&g_settings);
  backlight_set(backlight_get()); /* keep the dimmer untouched */
  s_rebuild_pending = true;       /* restyled from ui_poll(), outside the event */
}

static void save_click_cb(lv_event_t *e) {
  (void)e;
  if (s_ta_ssid) snprintf(g_settings.wifi_ssid, sizeof g_settings.wifi_ssid, "%s",
                          lv_textarea_get_text(s_ta_ssid));
  if (s_ta_pass) snprintf(g_settings.wifi_pass, sizeof g_settings.wifi_pass, "%s",
                          lv_textarea_get_text(s_ta_pass));
  /* floor / room come straight from the dropdowns and are already applied */
  for (int i = 0; i < GEN_SLOTS; i++) {
    if (!s_dd_dev[i]) continue;
    const uint16_t sel = lv_dropdown_get_selected(s_dd_dev[i]);
    if (sel == 0) {
      /* "(kosong)" -> unassign the button */
      g_settings.slots[i].guid[0] = '\0';
      g_settings.slots[i].label[0] = '\0';
    } else if (sel - 1 < (uint16_t)s_opt_count) {
      const int ri = s_opt_reg[sel - 1];
      snprintf(g_settings.slots[i].guid, sizeof g_settings.slots[i].guid, "%s",
               registry_guid_at(ri));
      snprintf(g_settings.slots[i].label, sizeof g_settings.slots[i].label, "%s",
               registry_shortname(ri));
    }
    if (s_dd_icon[i]) g_settings.slots[i].icon = (uint8_t)lv_dropdown_get_selected(s_dd_icon[i]);
    if (sel == 0) g_settings.slots[i].alias[0] = '\0'; /* unassigned -> drop alias */
  }
  settings_save(&g_settings);
  wifi_apply(g_settings.wifi_ssid, g_settings.wifi_pass);
  refresh_title();
  rebuild_dropdowns(); /* floor/room may have changed, so re-filter */
  for (int i = 0; i < GEN_SLOTS; i++) {
    lv_label_set_text(s_name[i], g_settings.slots[i].label);
    refresh_slot_visual(i);
  }
  lv_scr_load(s_main);
  ui_refresh_states();
}

/* ------------------------------------------------------------------------- */
/*  Diagnostics                                                              */
/* ------------------------------------------------------------------------- */
static int kb_rows(lv_obj_t *kb) {
  const char **map = lv_btnmatrix_get_map(kb);
  if (!map) return 0;
  int rows = 0;
  bool have = false;
  for (int i = 0; map[i]; i++) {
    if (map[i][0] == '\0') break; /* end of the map array, not a real button */
    if (!strcmp(map[i], "\n")) {
      rows++;
      have = false;
    } else {
      have = true;
    }
  }
  if (have) rows++;
  return rows;
}

/* Report the colours actually applied to the top-bar icons (and the palette
 * entries they used to borrow). Console command: "icons". */
void ui_log_icons(void) {
  const UiTheme *t = T();
  Serial.printf("[ui] tema=%s ok=#%06X accent=#%06X bad=#%06X dim=#%06X\n", t->name, t->ok,
                t->accent, t->bad, t->dim);
  lv_obj_t *objs[3] = {s_ic_wifi, s_ic_mqtt, s_ic_set};
  const char *nm[3] = {"wifi", "mqtt", "gear"};
  for (int i = 0; i < 3; i++) {
    if (!objs[i]) { Serial.printf("[ui] ic-%s: (tidak ada)\n", nm[i]); continue; }
    /* lv_color_t is RGB565 here: ch.red is 5 bits, ch.green 6, ch.blue 5.
     * Printing the raw fields looked like a much darker colour than the panel
     * shows, so widen to 24-bit first. */
    const lv_color_t c = lv_obj_get_style_text_color(objs[i], 0);
    const uint32_t rgb = (((uint32_t)c.ch.red << 3) << 16) |
                         (((uint32_t)c.ch.green << 2) << 8) | ((uint32_t)c.ch.blue << 3);
    Serial.printf("[ui] ic-%s live=#%06X\n", nm[i], (unsigned)rgb);
  }
  Serial.printf("[ui] wifi_ok=%d mqtt=%d harap up=#%06X down=#%06X\n", (int)wifi_ok(),
                (int)mqtt_connected(), LINK_UP_HEX, LINK_DOWN_HEX);
}
void ui_log_geometry(void) {
  Serial.printf("[ui] grid cols=%d rows=%d slots=%d btn=%dx%d row_h=%d\n", UI_COLS,
                UI_ROWS, UI_SLOTS, (int)BTN_W, (int)BTN_H, (int)SLOT_ROW_H);
  lv_obj_t *tics[3] = {s_ic_wifi, s_ic_mqtt, s_ic_set};
  const char *tnm[3] = {"wifi", "mqtt", "gear"};
  for (int k = 0; k < 3; k++) {
    if (!tics[k]) continue;
    lv_obj_t *b = lv_obj_get_parent(tics[k]);
    if (!b) b = tics[k];
    lv_area_t a;
    lv_obj_get_coords(b, &a);
    Serial.printf("[ui] ic-%s pos=%d,%d size=%dx%d right=%d\n", tnm[k], (int)a.x1,
                  (int)a.y1, (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1), (int)a.x2);
  }
  if (s_title) {
    lv_area_t a;
    lv_obj_get_coords(s_title, &a);
    Serial.printf("[ui] top-title pos=%d,%d size=%dx%d\n", (int)a.x1, (int)a.y1,
                  (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1));
  }
  for (int i = 0; i < UI_SLOTS && i < GEN_SLOTS; i++) {
    if (!s_btn[i]) continue;
    lv_area_t a;
    lv_obj_get_coords(s_btn[i], &a);
    Serial.printf("[ui] btn%d pos=%d,%d size=%dx%d bottom=%d\n", i, (int)a.x1, (int)a.y1,
                  (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1), (int)a.y2);
  }
  lv_obj_t *kbs[2] = {s_kb, s_edit_kb};
  for (int k = 0; k < 2; k++) {
    lv_obj_t *kb = kbs[k];
    if (!kb) continue;
    lv_area_t a;
    lv_obj_get_coords(kb, &a);
    Serial.printf("[ui] kb%d pos=%d,%d size=%dx%d content_h=%d rows=%d\n", k, (int)a.x1,
                  (int)a.y1, (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1),
                  (int)lv_obj_get_content_height(kb), kb_rows(kb));
  }
  lv_obj_t *objs[3] = {s_edit, s_edit_ta, s_edit_lbl};
  const char *names[3] = {"overlay", "field", "title"};
  for (int k = 0; k < 3; k++) {
    if (!objs[k]) continue;
    lv_area_t a;
    lv_obj_get_coords(objs[k], &a);
    Serial.printf("[ui] %s pos=%d,%d size=%dx%d\n", names[k], (int)a.x1, (int)a.y1,
                  (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1));
  }
  Serial.printf("[ui] screen=%dx%d\n", (int)UI_W, (int)UI_H);
}

static void mk_settings(void) {
  s_set = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s_set, C(bg), 0);
  lv_obj_set_style_bg_opa(s_set, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_set, LV_OBJ_FLAG_SCROLLABLE);

  /* scrollable content */
  s_set_content = lv_obj_create(s_set);
  lv_obj_set_size(s_set_content, UI_W, SET_H);
  lv_obj_set_pos(s_set_content, 0, 0);
  lv_obj_set_style_bg_opa(s_set_content, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_set_content, 0, 0);
  lv_obj_set_style_pad_all(s_set_content, 8, 0);
  lv_obj_set_style_pad_row(s_set_content, 6, 0);
  lv_obj_set_flex_flow(s_set_content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(s_set_content, LV_DIR_VER);

  mk_label(s_set_content, "SETELAN", F_TITLE, C(text));

  /* ---- WiFi ---- */
  mk_label(s_set_content, "WiFi", &lv_font_montserrat_14, C(accent));
  mk_ta(s_set_content, &s_ta_ssid, g_settings.wifi_ssid, false);
  mk_ta(s_set_content, &s_ta_pass, g_settings.wifi_pass, true);
  lv_obj_add_event_cb(s_ta_ssid, ta_event_cb, LV_EVENT_FOCUSED, NULL);
  lv_obj_add_event_cb(s_ta_ssid, ta_event_cb, LV_EVENT_DEFOCUSED, NULL);
  lv_obj_add_event_cb(s_ta_pass, ta_event_cb, LV_EVENT_FOCUSED, NULL);
  lv_obj_add_event_cb(s_ta_pass, ta_event_cb, LV_EVENT_DEFOCUSED, NULL);
  lv_obj_add_event_cb(s_ta_ssid, ta_click_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(s_ta_pass, ta_click_cb, LV_EVENT_CLICKED, NULL);
  s_lbl_wifi = mk_label(s_set_content, wifi_state_text(), &lv_font_montserrat_12, C(muted));

  /* ---- title: floor + room dropdowns (also drive the device filter) ---- */
  mk_label(s_set_content, "Tampilan (judul)", &lv_font_montserrat_14, C(accent));
  lv_obj_t *row1 = mk_row(s_set_content, SET_ROW_H);

  lv_obj_t *ddf = lv_dropdown_create(row1);
  lv_obj_set_width(ddf, UI_BIG ? 216 : 148);
  lv_obj_set_height(ddf, SET_ROW_H);
  lv_obj_set_style_text_font(ddf, &lv_font_montserrat_12, 0);
  lv_obj_set_style_bg_color(ddf, C(card2), 0);
  lv_obj_set_style_text_color(ddf, C(text), 0);
  lv_obj_add_event_cb(ddf, floor_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);
  s_dd_floor = ddf;

  lv_obj_t *ddr = lv_dropdown_create(row1);
  lv_obj_set_width(ddr, UI_BIG ? 216 : 148);
  lv_obj_set_height(ddr, SET_ROW_H);
  lv_obj_set_style_text_font(ddr, &lv_font_montserrat_12, 0);
  lv_obj_set_style_bg_color(ddr, C(card2), 0);
  lv_obj_set_style_text_color(ddr, C(text), 0);
  lv_obj_add_event_cb(ddr, room_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);
  s_dd_room = ddr;

  /* ---- screen: brightness + theme ---- */
  mk_label(s_set_content, "Layar & tema", &lv_font_montserrat_14, C(accent));

  lv_obj_t *brow = mk_row(s_set_content, SET_ROW_H);
  mk_label(brow, "Terang", &lv_font_montserrat_12, C(muted));

  lv_obj_t *minus = lv_btn_create(brow);
  lv_obj_set_size(minus, BSTEP_W, SET_ROW_H);
  lv_obj_set_style_radius(minus, 8, 0);
  lv_obj_set_style_bg_color(minus, C(card), 0);
  lv_obj_set_style_border_width(minus, 1, 0);
  lv_obj_set_style_border_color(minus, C(border), 0);
  lv_obj_add_event_cb(minus, bright_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-10);
  mk_label(minus, "-", &lv_font_montserrat_20, C(text));

  lv_obj_t *sl = lv_slider_create(brow);
  lv_obj_set_width(sl, SLIDER_W);
  lv_obj_set_height(sl, UI_BIG ? 22 : 18);
  lv_slider_set_range(sl, 20, 100);
  lv_slider_set_value(sl, g_settings.brightness, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sl, C(card2), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(sl, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, C(accent), LV_PART_INDICATOR);
  lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, C(text), LV_PART_KNOB);
  lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, 3, LV_PART_KNOB);
  lv_obj_add_event_cb(sl, bright_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(sl, bright_slider_cb, LV_EVENT_RELEASED, NULL);
  s_slider = sl;

  lv_obj_t *plus = lv_btn_create(brow);
  lv_obj_set_size(plus, BSTEP_W, SET_ROW_H);
  lv_obj_set_style_radius(plus, 8, 0);
  lv_obj_set_style_bg_color(plus, C(card), 0);
  lv_obj_set_style_border_width(plus, 1, 0);
  lv_obj_set_style_border_color(plus, C(border), 0);
  lv_obj_add_event_cb(plus, bright_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)10);
  mk_label(plus, "+", &lv_font_montserrat_20, C(text));

  char btxt[16];
  snprintf(btxt, sizeof btxt, "%u%%", (unsigned)g_settings.brightness);
  s_lbl_bright = mk_label(brow, btxt, &lv_font_montserrat_12, C(text));
  lv_obj_set_width(s_lbl_bright, VAL_W);

  lv_obj_t *trow = mk_row(s_set_content, SET_ROW_H);
  mk_label(trow, "Tema", &lv_font_montserrat_12, C(muted));
  lv_obj_t *ddt = lv_dropdown_create(trow);
  lv_obj_set_width(ddt, DD_W);
  lv_obj_set_height(ddt, SET_ROW_H);
  lv_obj_set_style_text_font(ddt, &lv_font_montserrat_12, 0);
  lv_obj_set_style_bg_color(ddt, C(card2), 0);
  lv_obj_set_style_text_color(ddt, C(text), 0);
  lv_dropdown_set_options(ddt, theme_names());
  lv_dropdown_set_selected(ddt, g_settings.theme);
  lv_obj_add_event_cb(ddt, theme_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);
  s_dd_theme = ddt;

  /* touch calibration from the panel itself */
  lv_obj_t *crow = mk_row(s_set_content, SET_ROW_H);
  lv_obj_t *bcal = lv_btn_create(crow);
  lv_obj_set_size(bcal, DD_W, SET_ROW_H);
  lv_obj_set_style_bg_color(bcal, C(card), 0);
  lv_obj_set_style_border_width(bcal, 1, 0);
  lv_obj_set_style_border_color(bcal, C(border), 0);
  lv_obj_add_event_cb(bcal, cal_btn_cb, LV_EVENT_CLICKED, NULL);
  mk_label(bcal, "Kalibrasi sentuh", &lv_font_montserrat_12, C(text));
  mk_label(crow, "ketuk 4 kotak + tengah", &lv_font_montserrat_12, C(muted));

  /* ---- slot assignment ---- */
  mk_label(s_set_content, "Tombol lampu (id - ikon - nama)", &lv_font_montserrat_14,
           C(accent));

  /* filter row: which devices the dropdowns offer */
  lv_obj_t *srow = mk_row(s_set_content, SET_ROW_H);
  mk_label(srow, "Filter", &lv_font_montserrat_12, C(muted));
  lv_obj_t *dds = lv_dropdown_create(srow);
  lv_obj_set_width(dds, DD_W);
  lv_obj_set_height(dds, SET_ROW_H);
  lv_obj_set_style_text_font(dds, &lv_font_montserrat_12, 0);
  lv_obj_set_style_bg_color(dds, C(card2), 0);
  lv_obj_set_style_text_color(dds, C(text), 0);
  lv_dropdown_set_options(dds, SCOPE_NAMES);
  lv_dropdown_set_selected(dds, g_settings.scope);
  lv_obj_add_event_cb(dds, scope_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);
  s_dd_scope = dds;
  s_lbl_scope = mk_label(srow, "", &lv_font_montserrat_12, C(muted));
  lv_obj_set_width(s_lbl_scope, UI_BIG ? 110 : 96);

  lv_obj_t *s_lbl_hint = mk_label(s_set_content, "Nama (alias) kosong = otomatis dari id",
                                  &lv_font_montserrat_12, C(muted));
  (void)s_lbl_hint;

  for (int i = 0; i < UI_SLOTS; i++) {
    lv_obj_t *row = mk_row(s_set_content, SET_ROW_H);

    char num[8];
    snprintf(num, sizeof num, "%d", i + 1);
    mk_label(row, num, &lv_font_montserrat_14, C(muted));

    lv_obj_t *dd1 = lv_dropdown_create(row);
    lv_obj_set_width(dd1, DD_DEV_W);
    lv_obj_set_height(dd1, SET_ROW_H);
    lv_obj_set_style_text_font(dd1, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_left(dd1, 8, 0);
    lv_obj_set_style_pad_right(dd1, 18, 0);
    lv_obj_set_style_bg_color(dd1, C(card2), 0);
    lv_obj_set_style_text_color(dd1, C(text), 0);
    lv_obj_add_event_cb(dd1, dev_dd_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    s_dd_dev[i] = dd1;

    lv_obj_t *dd2 = lv_dropdown_create(row);
    lv_obj_set_width(dd2, DD_ICON_W);
    lv_obj_set_height(dd2, SET_ROW_H);
    lv_obj_set_style_text_font(dd2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(dd2, C(card2), 0);
    lv_obj_set_style_text_color(dd2, C(text), 0);
    lv_dropdown_set_options(dd2, ICON_NAMES);
    lv_dropdown_set_selected(dd2, g_settings.slots[i].icon % ICON_COUNT);
    s_dd_icon[i] = dd2;

    /* friendly name: tap the field to rename it */
    char eff[24];
    slot_display_name(i, eff, sizeof eff);
    lv_obj_t *nb = lv_btn_create(row);
    lv_obj_set_size(nb, ALIAS_W, SET_ROW_H);
    lv_obj_set_style_radius(nb, 8, 0);
    lv_obj_set_style_bg_color(nb, C(card2), 0);
    lv_obj_set_style_border_width(nb, 1, 0);
    lv_obj_set_style_border_color(nb, C(border), 0);
    lv_obj_set_style_pad_all(nb, 0, 0);
    lv_obj_add_event_cb(nb, alias_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *nl = mk_label(nb, eff, &lv_font_montserrat_12, C(text));
    lv_obj_set_width(nl, 128);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
    lv_obj_align(nl, LV_ALIGN_LEFT_MID, 10, 0);
    s_lbl_alias[i] = nl;
  }

  /* ---- bottom bar (safe interior zone) ---- */
  lv_obj_t *bar = lv_obj_create(s_set);
  lv_obj_set_size(bar, LV_PCT(100), BAR_H);
  lv_obj_set_pos(bar, 0, UI_H - BAR_H);
  lv_obj_set_style_bg_color(bar, C(bg), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 4, 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *back = lv_btn_create(bar);
  lv_obj_set_size(back, 150, 38);
  lv_obj_set_style_bg_color(back, C(card), 0);
  lv_obj_set_style_border_width(back, 1, 0);
  lv_obj_set_style_border_color(back, C(border), 0);
  lv_obj_add_event_cb(back, back_click_cb, LV_EVENT_CLICKED, NULL);
  mk_label(back, "< Kembali", &lv_font_montserrat_14, C(text));

  lv_obj_t *save = lv_btn_create(bar);
  lv_obj_set_size(save, 150, 38);
  lv_obj_set_style_bg_color(save, C(accent), 0);
  lv_obj_add_event_cb(save, save_click_cb, LV_EVENT_CLICKED, NULL);
  mk_label(save, "Simpan", &lv_font_montserrat_14, C(text));

  /* keyboard (hidden until a text field is tapped) */
  s_kb = lv_keyboard_create(s_set);
  lv_obj_set_size(s_kb, LV_PCT(100), KB_MAIN_H);
  /* anchor to the bottom edge instead of an absolute y: absolute positions on
   * this screen came out shifted, which clipped the lower keyboard rows */
  lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(s_kb, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(s_kb, 1, 0);
  lv_obj_set_style_pad_row(s_kb, 2, 0);
  lv_obj_set_style_pad_column(s_kb, 2, 0);
  lv_obj_set_style_text_font(s_kb, &lv_font_montserrat_14, 0);
  lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);

  /* ---- alias editor overlay (hidden) ---- */
  s_edit = lv_obj_create(s_set);
  lv_obj_set_size(s_edit, UI_W, UI_H);
  lv_obj_set_pos(s_edit, 0, 0);
  lv_obj_set_style_bg_color(s_edit, C(bg), 0);
  lv_obj_set_style_bg_opa(s_edit, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_edit, 0, 0);
  lv_obj_set_style_pad_all(s_edit, 0, 0); /* absolute child coordinates */
  lv_obj_clear_flag(s_edit, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_edit, LV_OBJ_FLAG_HIDDEN);

  s_edit_lbl = mk_label(s_edit, "Nama tombol", &lv_font_montserrat_14, C(text));
  lv_obj_set_pos(s_edit_lbl, 10, 0);

  s_edit_ta = lv_textarea_create(s_edit);
  lv_obj_set_size(s_edit_ta, UI_W - 16, EDIT_FIELD_H);
  lv_obj_set_pos(s_edit_ta, 8, EDIT_FIELD_Y);
  lv_textarea_set_one_line(s_edit_ta, true);
  lv_textarea_set_max_length(s_edit_ta, sizeof g_settings.slots[0].alias - 1);
  lv_obj_set_style_bg_color(s_edit_ta, C(card2), 0);
  lv_obj_set_style_text_color(s_edit_ta, C(text), 0);
  lv_obj_set_style_text_font(s_edit_ta, &lv_font_montserrat_16, 0);
  lv_obj_set_style_border_color(s_edit_ta, C(accent), 0);
  lv_obj_set_style_border_width(s_edit_ta, 2, 0);
  lv_obj_set_style_radius(s_edit_ta, 10, 0);
  lv_obj_set_style_pad_left(s_edit_ta, 10, 0);

  lv_obj_t *bcan = lv_btn_create(s_edit);
  lv_obj_set_size(bcan, UI_BIG ? 150 : 96, EDIT_BTN_H);
  lv_obj_set_pos(bcan, 8, EDIT_BTN_Y);
  lv_obj_set_style_bg_color(bcan, C(card), 0);
  lv_obj_set_style_border_width(bcan, 1, 0);
  lv_obj_set_style_border_color(bcan, C(border), 0);
  lv_obj_add_event_cb(bcan, edit_cancel_cb, LV_EVENT_CLICKED, NULL);
  mk_label(bcan, "Batal", &lv_font_montserrat_14, C(text));

  lv_obj_t *bclr = lv_btn_create(s_edit);
  lv_obj_set_size(bclr, UI_BIG ? 150 : 96, EDIT_BTN_H);
  lv_obj_set_pos(bclr, UI_BIG ? 165 : 112, EDIT_BTN_Y);
  lv_obj_set_style_bg_color(bclr, C(card), 0);
  lv_obj_set_style_border_width(bclr, 1, 0);
  lv_obj_set_style_border_color(bclr, C(border), 0);
  lv_obj_add_event_cb(bclr, edit_clear_cb, LV_EVENT_CLICKED, NULL);
  mk_label(bclr, "Hapus", &lv_font_montserrat_14, C(text));

  lv_obj_t *bok = lv_btn_create(s_edit);
  lv_obj_set_size(bok, UI_BIG ? 150 : 96, EDIT_BTN_H);
  lv_obj_set_pos(bok, UI_W - (UI_BIG ? 158 : 104), EDIT_BTN_Y);
  lv_obj_set_style_bg_color(bok, C(ok), 0);
  lv_obj_add_event_cb(bok, edit_ok_cb, LV_EVENT_CLICKED, NULL);
  mk_label(bok, "Simpan", &lv_font_montserrat_14, C(bg));

  s_edit_kb = lv_keyboard_create(s_edit);
  lv_obj_set_size(s_edit_kb, UI_W, KB_EDIT_H);
  lv_obj_align(s_edit_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(s_edit_kb, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(s_edit_kb, 1, 0);
  lv_obj_set_style_pad_row(s_edit_kb, 2, 0);
  lv_obj_set_style_pad_column(s_edit_kb, 2, 0);
  lv_obj_set_style_text_font(s_edit_kb, &lv_font_montserrat_14, 0);
  lv_keyboard_set_textarea(s_edit_kb, s_edit_ta);
}

/* ------------------------------------------------------------------------- */
/*  Device dropdowns: filter by floor / room, show the short id only.        */
/* ------------------------------------------------------------------------- */
static bool opt_has(int reg_idx) {
  for (int k = 0; k < s_opt_count; k++)
    if (s_opt_reg[k] == reg_idx) return true;
  return false;
}

static void opt_add(int reg_idx) {
  if (reg_idx < 0 || reg_idx >= registry_count()) return;
  if (s_opt_count >= REG_OPT_MAX || opt_has(reg_idx)) return;
  s_opt_reg[s_opt_count++] = reg_idx;
}

/* Build the visible device list for the current scope. Devices already used by
 * a button are always kept in the list, so a filter can never hide a slot. */
static void build_option_map(void) {
  const int n = registry_count();
  s_opt_count = 0;

  for (int k = 0; k < n; k++)
    if (registry_matches_scope(k, g_settings.scope, g_settings.floor_id, g_settings.room_id,
                               g_settings.floor_label, g_settings.room_label))
      opt_add(k);

  if (s_opt_count == 0) /* nothing matches -> show everything instead of nothing */
    for (int k = 0; k < n; k++) opt_add(k);

  for (int i = 0; i < GEN_SLOTS; i++)
    opt_add(registry_resolve(g_settings.slots[i].guid, g_settings.slots[i].label));
}

static void rebuild_dropdowns(void) {
  build_option_map();

  if (s_lbl_scope) {
    char b[40];
    snprintf(b, sizeof b, "%d dari %d", s_opt_count, registry_count());
    lv_label_set_text(s_lbl_scope, b);
  }

  /* Option text = short id, built on the heap so nothing is truncated. */
  size_t cap = 32;
  for (int k = 0; k < s_opt_count; k++) cap += strlen(registry_shortname(s_opt_reg[k])) + 1;
  char *opts = (char *)malloc(cap);
  if (!opts) return;
  strcpy(opts, "(kosong)");
  size_t len = strlen(opts);
  for (int k = 0; k < s_opt_count; k++) {
    const char *nm = registry_shortname(s_opt_reg[k]);
    const size_t l = strlen(nm);
    if (len + 1 + l + 1 > cap) break;
    opts[len++] = '\n';
    memcpy(opts + len, nm, l + 1);
    len += l;
  }

  for (int i = 0; i < GEN_SLOTS; i++) {
    if (s_dd_dev[i]) {
      lv_dropdown_set_options(s_dd_dev[i], opts);
      int sel = 0;
      const int ri = registry_resolve(g_settings.slots[i].guid, g_settings.slots[i].label);
      if (ri >= 0)
        for (int k = 0; k < s_opt_count; k++)
          if (s_opt_reg[k] == ri) { sel = k + 1; break; }
      lv_dropdown_set_selected(s_dd_dev[i], sel);
    }
    if (s_dd_icon[i]) {
      uint8_t ic = g_settings.slots[i].icon;
      lv_dropdown_set_selected(s_dd_icon[i], ic < ICON_COUNT ? ic : 0);
    }
  }
  free(opts);
}

/* Forget every widget handle before the screens are rebuilt. */
static void reset_widgets(void) {
  s_main = s_set = NULL;
  memset(s_last_name, 0, sizeof s_last_name);
  for (int i = 0; i < GEN_SLOTS; i++) s_last_state[i] = -1;
  s_last_icons = false;
  s_title = NULL;
  s_ic_wifi = s_ic_mqtt = s_ic_set = NULL;
  s_ta_ssid = s_ta_pass = NULL;
  s_dd_floor = s_dd_room = NULL;
  s_lbl_wifi = s_kb = s_set_content = NULL;
  s_slider = s_lbl_bright = s_dd_theme = NULL;
  s_dd_scope = s_lbl_scope = NULL;
  for (int i = 0; i < GEN_SLOTS; i++) {
    s_btn[i] = s_icon_box[i] = s_icon_glow[i] = NULL;
    s_name[i] = s_state[i] = NULL;
    s_dd_dev[i] = s_dd_icon[i] = s_lbl_alias[i] = NULL;
    for (int k = 0; k < 4; k++) s_icon_fill[i][k] = NULL;
    for (int k = 0; k < 2; k++) s_icon_line[i][k] = NULL;
  }
  s_edit = s_edit_lbl = s_edit_ta = s_edit_kb = NULL;
  s_edit_slot = -1;
}

/* Rebuild both screens from scratch — used when the theme changes. */
static void rebuild_all_ui(void) {
  const bool stay_settings = (lv_scr_act() == s_set);
  lv_obj_t *old_main = s_main;
  lv_obj_t *old_set = s_set;

  reset_widgets();
  lv_theme_default_init(lv_disp_get_default(), C(accent), C(ok), !T()->light,
                        LV_FONT_DEFAULT);
  mk_main();
  mk_settings();
  refresh_title();
  rebuild_location_dd();
  lv_scr_load(stay_settings ? s_set : s_main);
  if (stay_settings) rebuild_dropdowns();
  if (old_main) lv_obj_del(old_main);
  if (old_set) lv_obj_del(old_set);

  s_last_refresh = 0;
  ui_refresh_states();
}

void ui_registry_changed(void) { rebuild_dropdowns(); }

void ui_catalog_changed(void) {
  /* Older NVS blobs have no floor/room ids: derive them from the labels once
   * the catalog is available, then refresh both title dropdowns. */
  bool fixed = false;
  if (!g_settings.floor_id && g_settings.floor_label[0]) {
    const int fi = catalog_find_floor(0, g_settings.floor_label);
    if (fi >= 0) {
      g_settings.floor_id = catalog_floor_id(fi);
      fixed = true;
    }
  }
  if (!g_settings.room_id && g_settings.room_label[0]) {
    const int ri = catalog_find_room(g_settings.floor_id, 0, g_settings.room_label);
    if (ri >= 0) {
      g_settings.room_id = catalog_room_id(g_settings.floor_id, ri);
      fixed = true;
    }
  }
  if (fixed) settings_save(&g_settings);
  rebuild_location_dd();
  if (s_title) refresh_title();
  s_dd_dirty = true;
}

void ui_after_calibration(void) {
  /* the routine drew straight on the panel: repaint everything and go back
   * to the settings screen so the user can carry on */
  if (s_main) lv_obj_invalidate(s_main);
  if (s_set) lv_obj_invalidate(s_set);
  s_last_refresh = 0;
  ui_open_settings();
  ui_refresh_states();
}
void ui_theme_changed(void) { s_rebuild_pending = true; }

void ui_poll(void) {
  if (s_cal_request) {
    s_cal_request = false;
    touch_calibrate();
    return;
  }
  if (s_rebuild_pending) {
    s_rebuild_pending = false;
    rebuild_all_ui();
    return;
  }
  if (s_dd_dirty) {
    s_dd_dirty = false;
    rebuild_dropdowns();
  }
}

void ui_open_settings(void) {
  if (s_dd_scope) lv_dropdown_set_selected(s_dd_scope, g_settings.scope);
  refresh_alias_labels();
  rebuild_location_dd();
  rebuild_dropdowns();
  if (s_lbl_wifi) lv_label_set_text(s_lbl_wifi, wifi_state_text());
  if (s_slider) lv_slider_set_value(s_slider, g_settings.brightness, LV_ANIM_OFF);
  if (s_lbl_bright) {
    char b[16];
    snprintf(b, sizeof b, "%u%%", (unsigned)g_settings.brightness);
    lv_label_set_text(s_lbl_bright, b);
  }
  if (s_dd_theme) lv_dropdown_set_selected(s_dd_theme, g_settings.theme);
  lv_scr_load(s_set);
}

/* ------------------------------------------------------------------------- */

void ui_refresh_states(void) {
  if (millis() - s_last_refresh < 250) return;
  s_last_refresh = millis();

  /* top bar icons report link state: green = up, red = down (theme independent) */
  const bool wf = wifi_ok();
  const bool mq = mqtt_connected();
  if (!s_last_icons || wf != s_last_wifi) {
    if (s_ic_wifi)
      lv_obj_set_style_text_color(s_ic_wifi, lv_color_hex(wf ? LINK_UP_HEX : LINK_DOWN_HEX), 0);
    s_last_wifi = wf;
  }
  if (!s_last_icons || mq != s_last_mqtt) {
    if (s_ic_mqtt)
      lv_obj_set_style_text_color(s_ic_mqtt, lv_color_hex(mq ? LINK_UP_HEX : LINK_DOWN_HEX), 0);
    s_last_mqtt = mq;
  }
  if (!s_last_icons) {
    if (s_ic_set) lv_obj_set_style_text_color(s_ic_set, C(text), 0);
    s_last_icons = true;
  }
  if (s_lbl_wifi) lv_label_set_text(s_lbl_wifi, wifi_state_text());

  for (int i = 0; i < GEN_SLOTS; i++) {
    if (!s_btn[i]) continue;
    LampSlot *slot = &g_settings.slots[i];
    int ri = registry_resolve(slot->guid, slot->label);
    if (ri >= 0 && strcmp(slot->guid, registry_guid_at(ri)) != 0)
      snprintf(slot->guid, sizeof slot->guid, "%s", registry_guid_at(ri));
    bool known = ri >= 0 && registry_state_known(ri);
    bool on = known && registry_state_on(ri);
    const int8_t state = !known ? -1 : (on ? 1 : 0);

    char nm[24];
    slot_display_name(i, nm, sizeof nm);
    const bool name_changed = strcmp(nm, s_last_name[i]) != 0;
    if (name_changed) {
      lv_label_set_text(s_name[i], nm);
      snprintf(s_last_name[i], sizeof s_last_name[i], "%s", nm);
      lv_obj_set_style_text_color(s_name[i], ri >= 0 ? C(text) : C(muted), 0);
    }

    if (state != s_last_state[i]) {
      icon_apply_state(i, known, on);
      lv_label_set_text(s_state[i], !known ? "-" : (on ? "ON" : "OFF"));
      lv_obj_set_style_text_color(s_state[i],
                                  known ? (on ? C(state_on) : C(muted)) : C(dim), 0);
      lv_obj_set_style_bg_color(s_btn[i], on ? C(btn_on) : C(card), 0);
      lv_obj_set_style_border_color(s_btn[i], on ? C(btn_on_border) : C(border), 0);
      s_last_state[i] = state;
    }
  }
}

void ui_init(void) {
  lv_theme_default_init(lv_disp_get_default(), C(accent), C(ok), !T()->light,
                        LV_FONT_DEFAULT);
  reset_widgets();
  mk_main();
  mk_settings();
  refresh_title();
  rebuild_location_dd();
  lv_scr_load(s_main);
  ui_refresh_states();
}
