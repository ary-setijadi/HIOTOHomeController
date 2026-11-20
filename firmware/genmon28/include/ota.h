#pragma once
#include <stdbool.h>
#include <stdint.h>

/* ===========================================================================
 *  OTA over HTTP — pulls firmware from the Orange Pi OTA server
 *  ("ota-firmware" service on http://192.168.1.99:8080).
 *
 *  1. GET /api/latest?device=<type>&guid=<id>   -> manifest {latest_version, url}
 *  2. version differs -> GET <url> -> stream into Update -> reboot
 *  3. POST /api/report {status, from_version, to_version}
 * ======================================================================== */

void ota_begin(void);          /* boot bookkeeping + optional first check */
void ota_poll(void);           /* periodic check (respects ota_auto) */
void ota_request_check(void);  /* ask for a check from the next loop */
bool ota_check_now(void);      /* run a check right now (may reboot) */
bool ota_flash_url(const char *url); /* flash an explicit firmware URL */

const char *ota_version(void);
const char *ota_status_text(void);
