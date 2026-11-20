#pragma once
#include <Arduino.h>

/* ===========================================================================
 *  HIOTO General Monitor — build-time defaults
 * ======================================================================== */

/* MQTT broker — the live HIOTO controller's MQTT plugin. */
#define DEFAULT_MQTT_HOST "192.168.1.22"
#define DEFAULT_MQTT_PORT 1883
#define DEFAULT_MQTT_USER "/smarthome:smarthome"
#define DEFAULT_MQTT_PASS "Ssm4rt2!"

/* HIOTO controller HTTP API — used for the actuator list + state snapshots. */
#define DEFAULT_CTRL_HOST "192.168.1.22"
#define DEFAULT_CTRL_PORT 8081

/* OTA firmware source: the Orange Pi "ota-firmware" service (port 8080). */
#define OTA_BASE "http://192.168.1.99:8080"

/* Firmware version — supplied by each PlatformIO env, NOT shared.
 *
 * The 2.8" (ESP32-2432S028 / ST7789) and 3.5" (ESP32-3248S035 / ST7796) panels
 * are different hardware: different controller, different touch topology,
 * different button count, and separate OTA channels (FW_DEVICE_TYPE). They
 * therefore carry INDEPENDENT version series and independent release cadence.
 *
 * Series convention: <panel-major.minor>.<patch>
 *     genmon_28_st7789 -> 2.8.x      (panel width 320, GEN_SLOTS 6)
 *     genmon_35_st7796 -> 3.5.x      (panel width 480, GEN_SLOTS 8)
 *
 * Bump one series only when that panel's firmware changes. Never bump one to
 * "keep them in step" — that is exactly the coupling this split removes.
 */
#ifndef FW_VERSION
#define FW_VERSION "0.0.0-UNSET" /* a build reached without -DFW_VERSION */
#endif
/* One device type per board AND panel controller, so a board can only ever pull
 * an image built for its own glass. Set by the PlatformIO env:
 *   genmon_35_st7796 -> "hioto-genmon-35-st7796"
 *   genmon_28_st7789 -> "hioto-genmon-28-st7789"
 * The fallback is deliberately obvious so a build without the env flag is
 * spotted immediately on the console instead of silently joining a channel. */
#ifndef FW_DEVICE_TYPE
#define FW_DEVICE_TYPE "hioto-genmon-UNSET"
#endif
/* Human-readable panel controller for banners and release manifests. */
#ifndef FW_PANEL_CHIP
#define FW_PANEL_CHIP "UNKNOWN"
#endif
#define OTA_CHECK_MS 900000UL /* 15 min between automatic checks */

/* ---------------------------------------------------------------------------
 *  Self-healing: watchdog + Wi-Fi/MQTT supervision
 *
 *  A CYD panel is often out of reach (ceiling / wall mount), so a stalled loop
 *  or a dead Wi-Fi association cannot be fixed by pulling the plug. Two layers:
 *
 *  1. Task watchdog - a panic + reboot after WDT_TIMEOUT_S without a feed.
 *  2. Association supervisor - after a loss, reconnect every WIFI_RETRY_MS;
 *     a full re-init (Wi-Fi off/on) after WIFI_REINIT_MS; and a self-reboot
 *     after WIFI_REBOOT_MS, because a rebooted radio is cheaper than a dark
 *     panel. The reboot is only used once the board has been up at least once
 *     since power-on - a boot without an AP must not turn into a reboot loop.
 *
 *  MQTT gets the same treatment: if it stays down while Wi-Fi is healthy for
 *  MQTT_STALL_CYCLE_MS, cycle the Wi-Fi link once; after MQTT_STALL_REBOOT_MS,
 *  reboot. Broker credential rejections (rc 4/5) are NOT retried with a reboot
 *  - that is a configuration error and rebooting would only churn the fleet.
 * ------------------------------------------------------------------------ */
#define WDT_TIMEOUT_S 30UL         /* loop-task watchdog timeout */
#define WIFI_RETRY_MS 30000UL      /* reconnect attempt cadence while down */
#define WIFI_REINIT_MS 180000UL    /* full Wi-Fi re-init after 3 min down */
#define WIFI_REBOOT_MS 900000UL    /* self-reboot after 15 min down */
#define MQTT_STALL_CYCLE_MS 120000UL  /* MQTT silent >2 min -> cycle Wi-Fi once */
#define MQTT_STALL_REBOOT_MS 1800000UL /* MQTT dead >30 min -> reboot */

/* Device registration with the controller (POST /api/register). */
#define IDENTITY_RETRY_MS 300000UL /* retry every 5 min until registered */

/* Empty -> device id derived from the MAC ("hioto-genmon-xxxxxx"). */
#define DEFAULT_DEVICE_ID ""

/* Title shown on the main screen. */
#define DEFAULT_FLOOR "Lantai 1"
#define DEFAULT_ROOM  "Living Room"

/* ---------------------------------------------------------------------------
 *  Button grid — ONE source of truth
 *
 *  GEN_SLOTS is the number of lamp buttons AND the number of slot rows in
 *  Settings AND the size of every slot array. It is fixed per panel by the
 *  PlatformIO env, never inferred at runtime:
 *
 *    genmon_35_st7796  480x320  ->  -DGEN_SLOTS=8   (4 columns x 2 rows)
 *    genmon_28_st7789  320x240  ->  -DGEN_SLOTS=6   (3 columns x 2 rows)
 *
 *  Geometry is derived from it, so the grid and the settings list can never
 *  disagree about how many buttons a board has.
 * ------------------------------------------------------------------------ */
#ifndef GEN_SLOTS
#define GEN_SLOTS 8 /* fallback: the 3.5" layout */
#endif

#define GEN_ROWS 2
#if (GEN_SLOTS % GEN_ROWS) != 0
#error "GEN_SLOTS must be a positive multiple of GEN_ROWS"
#endif
#define GEN_COLS (GEN_SLOTS / GEN_ROWS)

/* Default slot assignment (short HIOTO lamp ids, resolved from the registry).
 * Must contain exactly GEN_SLOTS entries. */
#if GEN_SLOTS == 6
#define DEFAULT_SLOT_LABELS "LAMP.14", "LAMP.15", "LAMP.09", "LAMP.10", \
                            "LAMP.11", "LAMP.12"
#elif GEN_SLOTS == 8
#define DEFAULT_SLOT_LABELS "LAMP.14", "LAMP.15", "LAMP.09", "LAMP.10", \
                            "LAMP.11", "LAMP.12", "LAMP.13", "LAMP.17"
#else
#error "GEN_SLOTS has no DEFAULT_SLOT_LABELS - add one for this panel size"
#endif

/* Version series this build must belong to, derived from the panel it was
 * compiled for. Used only as a build-time sanity check (see board_tag_check):
 * a 2.8" image must never carry a 3.5.x version and vice versa, because the
 * two series advance independently. */
#if GEN_SLOTS == 6
#define FW_SERIES "2.8"
#elif GEN_SLOTS == 8
#define FW_SERIES "3.5"
#endif

/* Lamp icon styles offered in Settings (index = stored value). */
#define ICON_COUNT 5
#define ICON_NAMES "Spot\nUmum\nTL\nDownlight\nTaman"

/* Default colour theme index (see theme.cpp) + default backlight level. */
#define DEFAULT_THEME 0
#define DEFAULT_BRIGHTNESS 90

/* Device picker filter: 0 = same room, 1 = same floor, 2 = every actuator. */
#define DEFAULT_SCOPE 0
#define SCOPE_NAMES "Ruang ini\nLantai ini\nSemua device"

/* Default TFT_eSPI rotation. Both CYD panels store their image in portrait
 * (240x320 / 320x480), so rotation 1 gives landscape; a board whose panel is
 * mounted the other way can be fixed at runtime with the "disp" command. */
#define DEFAULT_ROT 1

/* MQTT topics */
#define TOPIC_STATUS    "iot/%s/status"
#define TOPIC_TELEMETRY "iot/%s/telemetry"

/* How often telemetry is published while connected. */
#define TELEMETRY_PERIOD_MS 15000

/* HIOTO heartbeat cadence (keeps the device online in the controller). */
#define HIOTO_HEARTBEAT_MS 30000

/* Refresh the controller registry (lamp list + state snapshot) every 60 s. */
#define REGISTRY_REFRESH_MS 60000
