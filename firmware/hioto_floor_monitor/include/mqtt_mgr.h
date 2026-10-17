#pragma once
#include <Arduino.h>
#include "settings.h"

/* MQTT client manager (PubSubClient over plain TCP).
 * Topic conventions (see config.h / README):
 *   iot/{id}/telemetry  device -> broker, JSON floor-monitor report
 *   iot/{id}/commands   broker -> device, JSON commands
 *   iot/{id}/status     device -> broker, retained online/offline (LWT) */
void mqtt_mgr_begin(const Settings *s);
void mqtt_mgr_poll(void);           /* connect / keepalive / periodic telemetry */
bool mqtt_mgr_connected(void);
void mqtt_mgr_publish_state(void);  /* publish telemetry now */
const char *mqtt_mgr_status_text(void);
