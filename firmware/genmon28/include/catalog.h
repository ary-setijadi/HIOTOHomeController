#pragma once
#include <Arduino.h>

/* ===========================================================================
 *  Floor + room catalog from the HIOTO controller.
 *
 *    GET /api/floors -> [{"id":3,"name":"Lantai 1"}, ...]
 *    GET /api/rooms  -> [{"id":8,"name":"Living Room","floor_id":3}, ...]
 *
 *  Used by the settings screen to pick the panel's [Floor] and [Room] from
 *  dropdowns instead of free text. Refreshed every few minutes; the firmware
 *  keeps working (with whatever was already fetched) when the API is down.
 * ======================================================================== */

#define CAT_MAX_FLOORS 16
#define CAT_MAX_ROOMS 80

void catalog_begin(void);
bool catalog_poll(void); /* true when the catalog content changed */
bool catalog_ready(void);

int catalog_floor_count(void);
const char *catalog_floor_name(int idx);
uint8_t catalog_floor_id(int idx);

/* floor_id == 0 lists every room. */
int catalog_room_count(uint8_t floor_id);
const char *catalog_room_name(uint8_t floor_id, int idx);
uint8_t catalog_room_id(uint8_t floor_id, int idx);

/* -1 when not found; name may be NULL/"" to match by id only. */
int catalog_find_floor(uint8_t id, const char *name);
int catalog_find_room(uint8_t floor_id, uint8_t id, const char *name);
