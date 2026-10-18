#include "theme.h"

/* ===========================================================================
 *  Six complete palettes.  Order matters: the stored index selects the entry,
 *  so new themes must be appended at the end to keep NVS values meaningful.
 * ======================================================================== */
const UiTheme g_themes[THEME_COUNT] = {
    /* 0 — Midnight: the original deep navy panel with amber lamps */
    {
        "Midnight",
        false,
        /* bg/card/card2/border */ 0x0B1220, 0x111A2E, 0x0E1626, 0x1E2A44,
        /* text/muted/dim        */ 0xE2E8F0, 0x7A8AA0, 0x4A5A70,
        /* accent/ok/warn/bad    */ 0x3B82F6, 0x22C55E, 0xF5B400, 0xEF4444,
        /* btn_on/border/state   */ 0x1E2E16, 0xFFC107, 0xFFC107,
        /* icon on               */ 0xFFC107, 0xFFE082,
        /* icon off              */ 0x4A5C77, 0x5A6B85,
        /* icon unknown          */ 0x3E4C63, 0x5A6B85,
        /* glow                  */ 0xFFF3C1,
    },
    /* 1 — Grafit: neutral graphite with cyan accents and warm lamps */
    {
        "Grafit",
        false,
        /* bg/card/card2/border */ 0x14171C, 0x1C2027, 0x161A20, 0x2C313A,
        /* text/muted/dim        */ 0xECEDEF, 0x8E97A3, 0x5C646F,
        /* accent/ok/warn/bad    */ 0x38BDF8, 0x34D399, 0xFFC857, 0xF87171,
        /* btn_on/border/state   */ 0x24282F, 0xFFC857, 0xFFD479,
        /* icon on               */ 0xFFC857, 0xFFE3A3,
        /* icon off              */ 0x59616C, 0x6E7883,
        /* icon unknown          */ 0x474E58, 0x626B77,
        /* glow                  */ 0xFFF6D5,
    },
    /* 2 — Samudra: deep teal with cool cyan lamps */
    {
        "Samudra",
        false,
        /* bg/card/card2/border */ 0x06202A, 0x0B2C38, 0x082530, 0x15454F,
        /* text/muted/dim        */ 0xDCF3F7, 0x7FA8B4, 0x4E7A88,
        /* accent/ok/warn/bad    */ 0x14B8A6, 0x2DD4BF, 0xFBBF24, 0xF87171,
        /* btn_on/border/state   */ 0x0C3A46, 0x67E8F9, 0x67E8F9,
        /* icon on               */ 0x67E8F9, 0xA5F3FC,
        /* icon off              */ 0x3F6B78, 0x54808D,
        /* icon unknown          */ 0x2F5561, 0x4A7480,
        /* glow                  */ 0xE0FBFF,
    },
    /* 3 — Senja: warm plum dusk with orange lamps */
    {
        "Senja",
        false,
        /* bg/card/card2/border */ 0x1A0F1F, 0x261530, 0x1F1027, 0x3A2247,
        /* text/muted/dim        */ 0xF5E8F7, 0xA98BB8, 0x6E5580,
        /* accent/ok/warn/bad    */ 0xF97316, 0x84CC16, 0xFF9E3D, 0xEF4444,
        /* btn_on/border/state   */ 0x33202A, 0xFF9E3D, 0xFF9E3D,
        /* icon on               */ 0xFF9E3D, 0xFFC48A,
        /* icon off              */ 0x6B4E74, 0x856589,
        /* icon unknown          */ 0x543A60, 0x745279,
        /* glow                  */ 0xFFE7CC,
    },
    /* 4 — Terang: bright paper theme for sunlit rooms */
    {
        "Terang",
        true,
        /* bg/card/card2/border */ 0xF4F6FA, 0xFFFFFF, 0xEDF1F7, 0xD3DAE5,
        /* text/muted/dim        */ 0x1B2432, 0x63707F, 0x93A0AF,
        /* accent/ok/warn/bad    */ 0x2563EB, 0x16A34A, 0xD97706, 0xDC2626,
        /* btn_on/border/state   */ 0xFFF6E0, 0xE0A93C, 0xB45309,
        /* icon on               */ 0xF59E0B, 0xD97706,
        /* icon off              */ 0xB6BFCC, 0xC2CBD8,
        /* icon unknown          */ 0xD3DAE5, 0xE2E8F0,
        /* glow                  */ 0xFFF1C9,
    },
    /* 5 — Neon: black light with magenta lamps */
    {
        "Neon",
        false,
        /* bg/card/card2/border */ 0x05050A, 0x0E0B18, 0x0A0812, 0x2A1B45,
        /* text/muted/dim        */ 0xEADCFB, 0x9B7FC7, 0x63498A,
        /* accent/ok/warn/bad    */ 0xA855F7, 0x22D3EE, 0xE879F9, 0xFB7185,
        /* btn_on/border/state   */ 0x1E0F2B, 0xE879F9, 0xE879F9,
        /* icon on               */ 0xE879F9, 0xF5D0FE,
        /* icon off              */ 0x4B3568, 0x6B4D8C,
        /* icon unknown          */ 0x3B2A52, 0x5B4177,
        /* glow                  */ 0xFBE8FF,
    },
};

const UiTheme *theme_get(uint8_t idx) {
  if (idx >= THEME_COUNT) idx = 0;
  return &g_themes[idx];
}

const char *theme_names(void) {
  static const char kNames[] =
      "Midnight\nGrafit\nSamudra\nSenja\nTerang\nNeon";
  return kNames;
}
