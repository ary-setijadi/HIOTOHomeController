#pragma once
#include <stdbool.h>
#include <stdint.h>

/* ===========================================================================
 *  Device identity for the HIOTO controller.
 *
 *  Every board gets a persistent UUID (generated once, stored in NVS) and is
 *  registered with POST /api/register together with its MAC, floor/room and a
 *  human name, so it can be identified in the controller device list next to
 *  its MAC address.
 * ======================================================================== */

void identity_begin(void);
bool identity_poll(void); /* true when a registration just succeeded */
bool identity_register_now(void);

const char *identity_guid(void);
const char *identity_state(void); /* "terdaftar" / "belum terdaftar" / ... */
bool identity_registered(void);
