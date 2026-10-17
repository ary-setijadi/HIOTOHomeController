#pragma once
#include <Arduino.h>

/* ===========================================================================
 *  HIOTO Secured Switch — build-time defaults
 *  Runtime overrides live in NVS (see settings.h); the provisioning portal
 *  allows changing Wi-Fi/MQTT/device settings without reflashing.
 * ======================================================================== */

/* MQTT broker defaults — matches the RabbitMQ MQTT plugin used by the
 * workspace's mqtt-simulator (topics iot/{id}/telemetry|commands|status). */
#define DEFAULT_MQTT_HOST "192.168.137.44"
#define DEFAULT_MQTT_PORT 1883
#define DEFAULT_MQTT_USER "admin"
#define DEFAULT_MQTT_PASS "CHANGE_ME"

/* Empty -> device id is derived from the MAC address ("hioto-xxxxxx"). */
#define DEFAULT_DEVICE_ID ""

/* Relay / switch output.
 * -1 = simulated output (no GPIO driven yet). When the real board output is
 * wired (SSR / relay driver / HLK-PM01 module), set the GPIO here or via the
 * provisioning portal (relay_gpio field). */
#define DEFAULT_RELAY_GPIO (-1)
#define RELAY_ACTIVE_LEVEL HIGH

/* Wi-Fi provisioning access point (captive portal). */
#define PROV_AP_SSID "HIOTO-Setup"
#define PROV_AP_PASS "CHANGE_ME" /* >= 8 chars */

/* MQTT topic templates; {id} is replaced with the device id. */
#define TOPIC_TELEMETRY "iot/%s/telemetry"
#define TOPIC_COMMANDS  "iot/%s/commands"
#define TOPIC_STATUS    "iot/%s/status"

/* How often telemetry is published while connected. */
#define TELEMETRY_PERIOD_MS 5000
