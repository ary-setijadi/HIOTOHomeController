#pragma once
#include <Arduino.h>

/* Floor device registry cache.
 *
 * The HIOTO controller (v4m) exposes GET /api/devices, a JSON array with
 * guid/name/floor_id per device. We fetch it once over HTTP, keep only the
 * devices of this panel's floor, and use their names in the dashboard.
 * Until the fetch succeeds the monitor falls back to showing whatever the
 * MQTT floor feed contains. */

/* Force a refetch on next poll (e.g. after settings change). */
void registry_begin(void);

/* Fetch when Wi-Fi is up and the registry isn't loaded yet.
 * Returns true on the poll that just completed a successful load. */
bool registry_poll(void);

bool registry_ready(void);

/* Human name for a guid, or NULL when the guid is not a node on this floor. */
const char *registry_name(const char *guid);

/* Whether a guid is part of this panel's floor. */
bool registry_contains(const char *guid);

int registry_count(void);

/* Room of a guid on this floor (derived from the device name, e.g.
 * "LAMP.12-1ST.FLOOR-LIVING ROOM-251-02" -> "Living Room"), or NULL. */
const char *registry_room_of(const char *guid);

/* Registry type "AKTUATOR" (lamp/relay) — such rows get an ON/OFF toggle. */
bool registry_is_actuator(const char *guid);

/* Unique room names discovered on this floor (insertion order). */
int registry_room_count(void);
const char *registry_room_name(int index);

const char *registry_status(void);
