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
#define FW_VERSION "2.0.0"
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

/* Device registration with the controller (POST /api/register). */
#define IDENTITY_RETRY_MS 300000UL /* retry every 5 min until registered */

/* Empty -> device id derived from the MAC ("hioto-genmon-xxxxxx"). */
#define DEFAULT_DEVICE_ID ""

/* Title shown on the main screen. */
#define DEFAULT_FLOOR "Lantai 1"
#define DEFAULT_ROOM  "Living Room"

/* Number of lamp buttons (2 rows x 4 columns). */
#define GEN_SLOTS 8

/* Default slot assignment (short HIOTO lamp ids, resolved from the registry). */
#define DEFAULT_SLOT_LABELS "LAMP.14", "LAMP.15", "LAMP.09", "LAMP.10", \
                            "LAMP.11", "LAMP.12", "LAMP.13", "LAMP.17"

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
