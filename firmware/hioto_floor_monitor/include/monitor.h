#pragma once
#include <Arduino.h>

/* ---------------------------------------------------------------------------
 *  HIOTO floor state model
 *  -----------------------
 *  The monitor subscribes to the broker and builds a live list of the nodes
 *  it can see (its own device, other CYD/simulator devices on iot/..., and the
 *  legacy HIOTO controller devices on Sensor/Status/sensor topics and Log).
 *  A node ages out (shown offline) after NODE_TIMEOUT_MS without traffic.
 * ------------------------------------------------------------------------- */

struct MonNode {
  char id[40];    /* guid / iot device id */
  char label[32]; /* human name (deviceName / device / id) */
  char room[24];  /* floor room (from the controller registry), "Lainnya" */
  char kind[16];  /* status | telemetry | sensor | relay */
  char value[64]; /* last value summary, e.g. "23.4C / 61%" */
  bool online;
  bool actuator;    /* registry type AKTUATOR -> row can be toggled */
  uint32_t last_ms; /* millis() of last traffic */
};

/* Parsed environment snapshot of the monitor itself. */
struct MonEnv {
  float die_temp_c;  /* SoC internal temperature (approx.) */
  int light_pct;     /* 0-100 from the LDR */
  int light_lux;     /* rough estimate for the UI */
  int rssi;          /* Wi-Fi RSSI */
  uint32_t uptime_s;
  uint32_t heap_free;
};

void monitor_init(void);

/* Feed one MQTT message (topic + payload) into the floor model. */
void monitor_feed(const char *topic, const uint8_t *payload, size_t len);

/* Age nodes out; call from loop(). */
void monitor_poll(void);

/* Re-apply registry labels/rooms to every tracked node (after a registry load). */
void monitor_apply_registry(void);

/* Create a placeholder node for every registered floor device, so devices that
 * are not publishing yet still appear in the list (offline, value "-"). */
void monitor_seed_from_registry(void);

/* Fill unknown actuator states from the controller's registry snapshot
 * (status "0" = ON, "1" = OFF, active-low). Real-time values always win. */
void monitor_apply_status_snapshot(void);

/* Remove nodes that are not part of this floor (call once the registry loads). */
void monitor_prune(void);

int monitor_count(void);
int monitor_online(void);
MonNode *monitor_node(int index);        /* NULL when out of range */
MonNode *monitor_find(const char *id);   /* NULL when unknown */

void monitor_env(MonEnv *out);

/* Build the iot/{id}/telemetry JSON body (NUL-terminated). */
void monitor_build_telemetry(char *buf, size_t len);
