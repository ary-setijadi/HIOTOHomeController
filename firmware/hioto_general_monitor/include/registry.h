#pragma once
#include <Arduino.h>

/* Lamp registry fetched from the HIOTO controller:
 * all AKTUATOR devices (short id + full name + last status) plus their
 * on/off state (learned from MQTT + registry status snapshots). */

void registry_begin(void);
bool registry_poll(void); /* true when a refresh just completed */
bool registry_ready(void);

int registry_count(void);
const char *registry_shortname(int index); /* e.g. "LAMP.12" */
const char *registry_guid_at(int index);
const char *registry_fullname(int index);
const char *registry_floor_at(int index); /* "1ST.FLOOR", "" when unknown */
const char *registry_room_at(int index);  /* "LIVING ROOM", "" when unknown */

/* Location scope used by the device picker: 0 = room, 1 = floor, 2 = all.
 * Controller floor/room ids win when the device carries them; otherwise the
 * floor/room text parsed out of the device name is compared with the labels. */
bool registry_matches_scope(int index, uint8_t scope, uint8_t floor_id, uint8_t room_id,
                            const char *floor_label, const char *room_label);

int registry_find_short(const char *shortname);
int registry_find_guid(const char *guid);

/* Resolve a stored slot: guid first, then the compact label as a fallback
 * (a saved guid can go stale when the controller re-registers a device). */
int registry_resolve(const char *guid, const char *label);

bool registry_state_known(int index);
bool registry_state_on(int index);
void registry_set_state(int index, bool on); /* marks known */

const char *registry_status(void);
