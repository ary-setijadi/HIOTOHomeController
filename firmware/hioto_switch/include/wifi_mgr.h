#pragma once
#include <Arduino.h>
#include "settings.h"

enum WifiState {
  WIFI_ST_OFF = 0, /* nothing started yet / STA failed and portal not yet up */
  WIFI_ST_CONNECTING,
  WIFI_ST_OK,
  WIFI_ST_PORTAL, /* SoftAP provisioning portal active */
};

/* Start station mode with stored credentials, or the provisioning portal when
 * no credentials are stored. Non-blocking. */
void wifi_mgr_begin(const Settings *s);

/* Drive the state machine; call from loop() every iteration. */
void wifi_mgr_poll(void);

WifiState wifi_mgr_state(void);
String wifi_mgr_ip(void); /* STA IP when connected, AP IP in portal mode */

/* Force the provisioning portal (e.g. BOOT long-press). */
void wifi_mgr_start_portal(void);

bool wifi_mgr_portal_active(void);
