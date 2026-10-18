#pragma once
#include <Arduino.h>
#include "settings.h"

/* MQTT client for the switch panel:
 *  - subscribes Aktuator/Sensor to learn lamp states
 *  - publishes HIOTO actuator commands (active-low: 0 = ON, 1 = OFF)
 *  - publishes retained iot/{id}/status, telemetry and a Status heartbeat */
void mqtt_begin(const Settings *s);
void mqtt_poll(void);
bool mqtt_connected(void);
const char *mqtt_state_text(void);

/* Publish "Aktuator" "<guid>#<0|1>". */
bool mqtt_publish_actuator(const char *guid, int value);
