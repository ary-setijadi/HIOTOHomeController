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

/* MQTT topics */
#define TOPIC_STATUS    "iot/%s/status"
#define TOPIC_TELEMETRY "iot/%s/telemetry"

/* How often telemetry is published while connected. */
#define TELEMETRY_PERIOD_MS 15000

/* HIOTO heartbeat cadence (keeps the device online in the controller). */
#define HIOTO_HEARTBEAT_MS 30000

/* Refresh the controller registry (lamp list + state snapshot) every 60 s. */
#define REGISTRY_REFRESH_MS 60000
