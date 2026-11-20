#pragma once
#include <stdbool.h>
#include <stdint.h>

/* ===========================================================================
 *  Colour themes for the switch panel.
 *
 *  Every theme carries the complete palette used by the UI so switching is
 *  a pure re-style (no widget logic depends on a specific theme).
 * ======================================================================== */

typedef struct {
  const char *name;
  bool light; /* true = light theme (affects the LVGL default theme mode) */

  /* surfaces */
  uint32_t bg;     /* screen background */
  uint32_t card;   /* buttons / bar surfaces */
  uint32_t card2;  /* input fields, slider track */
  uint32_t border; /* idle borders */

  /* type */
  uint32_t text;
  uint32_t muted;
  uint32_t dim;

  /* semantic */
  uint32_t accent;
  uint32_t ok;
  uint32_t warn;
  uint32_t bad;

  /* lamp ON state */
  uint32_t btn_on;        /* button fill when the lamp is ON */
  uint32_t btn_on_border; /* button border when the lamp is ON */
  uint32_t state_on;      /* "ON" text colour */

  /* lamp icon */
  uint32_t icon_on;      /* body fill, lamp ON */
  uint32_t icon_on_ring; /* body outline, lamp ON */
  uint32_t icon_off;     /* body fill, lamp OFF */
  uint32_t icon_off_ring;
  uint32_t icon_unk; /* body fill, state unknown */
  uint32_t icon_unk_ring;
  uint32_t glow; /* hot spot inside the lamp, shown when ON */
} UiTheme;

#define THEME_COUNT 6

extern const UiTheme g_themes[THEME_COUNT];

/* Resolve a stored theme index (out-of-range falls back to theme 0). */
const UiTheme *theme_get(uint8_t idx);

/* "\n"-separated theme names, ready for a LVGL dropdown. */
const char *theme_names(void);
