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

int registry_find_short(const char *shortname);
int registry_find_guid(const char *guid);

bool registry_state_known(int index);
bool registry_state_on(int index);
void registry_set_state(int index, bool on); /* marks known */

const char *registry_status(void);
