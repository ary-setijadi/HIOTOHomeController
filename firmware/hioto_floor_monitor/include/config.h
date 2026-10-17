#pragma once
#include <Arduino.h>

/* ===========================================================================
 *  HIOTO Floor Monitor — build-time defaults
 *  Runtime overrides live in NVS (see settings.h); the provisioning portal
 *  allows changing Wi-Fi/MQTT/floor settings without reflashing.
 * ======================================================================== */

/* MQTT broker defaults — the live HIOTO controller (v4m) MQTT plugin. */
#define DEFAULT_MQTT_HOST "192.168.1.22"
#define DEFAULT_MQTT_PORT 1883
#define DEFAULT_MQTT_USER "/smarthome:smarthome"
#define DEFAULT_MQTT_PASS "Ssm4rt2!"

/* HIOTO controller HTTP API (device registry / names). */
#define DEFAULT_CTRL_HOST "192.168.1.22"
#define DEFAULT_CTRL_PORT 8081
#define DEFAULT_FLOOR_ID 3 /* floors API: 1=Lantai 3, 2=Lantai 2, 3=Lantai 1, 4=Lantai 4 */

/* Empty -> device id is derived from the MAC address ("hioto-floor-xxxxxx"). */
#define DEFAULT_DEVICE_ID ""

/* Compound floor this panel represents. Editable in the portal or via MQTT. */
#define DEFAULT_FLOOR "Lantai 1"

/* Wi-Fi provisioning access point (captive portal). */
#define PROV_AP_SSID "HIOTO-Monitor-Setup"
#define PROV_AP_PASS "hioto1234" /* >= 8 chars */

/* MQTT topic templates; {id} is replaced with the device id. */
#define TOPIC_TELEMETRY "iot/%s/telemetry"
#define TOPIC_COMMANDS  "iot/%s/commands"
#define TOPIC_STATUS    "iot/%s/status"
#define TOPIC_SUHU      "sensor_suhu/%s" /* HIOTO legacy temperature topic */

/* How often telemetry is published while connected. */
#define TELEMETRY_PERIOD_MS 10000

/* HIOTO heartbeat / temperature cadence (Status + sensor_suhu). */
#define HIOTO_HEARTBEAT_MS 30000

/* A node that has not published within this window is shown as offline. */
#define NODE_TIMEOUT_MS 150000

/* Maximum nodes tracked on the floor dashboard. */
#define MON_MAX_NODES 32

/* Number of device-row widgets created (the list scrolls vertically). */
#define MON_ROWS_MAX 32
