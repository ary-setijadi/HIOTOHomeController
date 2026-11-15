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

/* Last CONNACK / socket result from connect_mqtt(): 0 = connected, 4/5 =
 * broker rejected the credentials, negative = network/socket failure. Lets the
 * supervisor tell "broker not reachable" (rebootable) from "wrong password"
 * (a configuration error, rebooting cannot help) and shows up in `status`. */
int mqtt_last_rc(void);

/* Drop the current session and retry on the next mqtt_poll() without waiting
 * for the back-off. Used when the Wi-Fi association just recovered. */
void mqtt_force_reconnect(void);

/* Publish "Aktuator" "<guid>#<0|1>". */
bool mqtt_publish_actuator(const char *guid, int value);
