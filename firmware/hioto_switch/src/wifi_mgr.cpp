#include "wifi_mgr.h"
#include "config.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

static WifiState s_state = WIFI_ST_OFF;
static const Settings *s_cfg = nullptr;
static uint32_t s_connect_started = 0;
static bool s_portal_active = false;

static WebServer s_server(80);
static DNSServer s_dns;
static const IPAddress kApIp(192, 168, 4, 1);
static const IPAddress kApMask(255, 255, 255, 0);

/* Pre-filled form values for the portal (current or default settings). */
static void portal_page() {
  Settings *s = &g_settings;
  String page = F(
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>HIOTO Setup</title></head>"
      "<body style='font-family:sans-serif;background:#0b1220;color:#e2e8f0;margin:0;padding:24px'>"
      "<h2 style='color:#3b82f6'>HIOTO Secured Switch — Type A</h2>"
      "<p>Configure Wi-Fi and MQTT. Values are stored in NVS and the device reboots.</p>"
      "<form method='POST' action='/save'>"
      "<p>Wi-Fi SSID<br><input name='ssid' value='");
  page += s->wifi_ssid;
  page += F("' style='width:100%;padding:8px;border-radius:8px;border:1px solid #1e2a44;background:#111a2e;color:#e2e8f0'></p>"
            "<p>Wi-Fi Password<br><input name='pass' type='password' value='");
  page += s->wifi_pass;
  page += F("' style='width:100%;padding:8px;border-radius:8px;border:1px solid #1e2a44;background:#111a2e;color:#e2e8f0'></p>"
            "<hr style='border-color:#1e2a44'>"
            "<p>MQTT Host<br><input name='mhost' value='");
  page += s->mqtt_host;
  page += F("' style='width:100%;padding:8px;border-radius:8px;border:1px solid #1e2a44;background:#111a2e;color:#e2e8f0'></p>"
            "<p>MQTT Port<br><input name='mport' type='number' value='");
  page += String(s->mqtt_port);
  page += F("' style='width:100%;padding:8px;border-radius:8px;border:1px solid #1e2a44;background:#111a2e;color:#e2e8f0'></p>"
            "<p>MQTT User<br><input name='muser' value='");
  page += s->mqtt_user;
  page += F("' style='width:100%;padding:8px;border-radius:8px;border:1px solid #1e2a44;background:#111a2e;color:#e2e8f0'></p>"
            "<p>MQTT Password<br><input name='mpass' type='password' value='");
  page += s->mqtt_pass;
  page += F("' style='width:100%;padding:8px;border-radius:8px;border:1px solid #1e2a44;background:#111a2e;color:#e2e8f0'></p>"
            "<hr style='border-color:#1e2a44'>"
            "<p>Device ID<br><input name='devid' value='");
  page += s->device_id;
  page += F("' style='width:100%;padding:8px;border-radius:8px;border:1px solid #1e2a44;background:#111a2e;color:#e2e8f0'></p>"
            "<p>Relay GPIO (-1 = simulated)<br><input name='relay' type='number' value='");
  page += String((int)s->relay_gpio);
  page += F("' style='width:100%;padding:8px;border-radius:8px;border:1px solid #1e2a44;background:#111a2e;color:#e2e8f0'></p>"
            "<p><button style='padding:12px 24px;border-radius:8px;border:0;background:#3b82f6;color:#fff;font-size:16px'>Save &apos;&amp;&apos; Reboot</button></p>"
            "</form></body></html>");
  s_server.send(200, "text/html; charset=utf-8", page);
}

static void portal_save() {
  Settings *s = &g_settings;
  if (s_server.hasArg("ssid")) strncpy(s->wifi_ssid, s_server.arg("ssid").c_str(), sizeof s->wifi_ssid - 1);
  if (s_server.hasArg("pass")) strncpy(s->wifi_pass, s_server.arg("pass").c_str(), sizeof s->wifi_pass - 1);
  if (s_server.hasArg("mhost")) strncpy(s->mqtt_host, s_server.arg("mhost").c_str(), sizeof s->mqtt_host - 1);
  if (s_server.hasArg("mport")) s->mqtt_port = (uint16_t)s_server.arg("mport").toInt();
  if (s_server.hasArg("muser")) strncpy(s->mqtt_user, s_server.arg("muser").c_str(), sizeof s->mqtt_user - 1);
  if (s_server.hasArg("mpass")) strncpy(s->mqtt_pass, s_server.arg("mpass").c_str(), sizeof s->mqtt_pass - 1);
  if (s_server.hasArg("devid")) strncpy(s->device_id, s_server.arg("devid").c_str(), sizeof s->device_id - 1);
  if (s_server.hasArg("relay")) s->relay_gpio = (int8_t)s_server.arg("relay").toInt();
  settings_save(s);

  s_server.send(200, "text/html; charset=utf-8",
                F("<html><body style='font-family:sans-serif;background:#0b1220;color:#e2e8f0;padding:24px'>"
                  "<h2 style='color:#22c55e'>Saved.</h2><p>Rebooting into station mode…</p></body></html>"));
  Serial.println("[wifi] settings saved from portal, rebooting");
  delay(1200);
  ESP.restart();
}

static void portal_redirect() {
  s_server.sendHeader("Location", String("http://") + kApIp.toString(), true);
  s_server.send(302, "text/plain", "");
}

void wifi_mgr_start_portal(void) {
  Serial.println("[wifi] starting provisioning portal (AP)");
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(kApIp, kApIp, kApMask);
  WiFi.softAP(PROV_AP_SSID, PROV_AP_PASS);
  delay(100);
  Serial.printf("[wifi] AP: %s  pass: %s  -> http://%s\n", PROV_AP_SSID,
                PROV_AP_PASS, kApIp.toString().c_str());

  s_dns.start(53, "*", kApIp);
  s_server.on("/", HTTP_GET, portal_page);
  s_server.on("/save", HTTP_POST, portal_save);
  s_server.onNotFound(portal_redirect);
  s_server.begin();
  s_portal_active = true;
  s_state = WIFI_ST_PORTAL;
}

void wifi_mgr_begin(const Settings *s) {
  s_cfg = s;
  if (s_cfg->wifi_ssid[0] == '\0') {
    Serial.println("[wifi] no stored credentials -> provisioning portal");
    wifi_mgr_start_portal();
    return;
  }
  Serial.printf("[wifi] connecting to '%s' ...\n", s_cfg->wifi_ssid);
  WiFi.mode(WIFI_STA);
  if (s_cfg->wifi_pass[0]) WiFi.begin(s_cfg->wifi_ssid, s_cfg->wifi_pass);
  else WiFi.begin(s_cfg->wifi_ssid);
  s_connect_started = millis();
  s_state = WIFI_ST_CONNECTING;
}

void wifi_mgr_poll(void) {
  switch (s_state) {
    case WIFI_ST_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[wifi] connected  ip=%s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        s_state = WIFI_ST_OK;
      } else if (millis() - s_connect_started > 25000) {
        Serial.println("[wifi] connect timeout -> provisioning portal");
        wifi_mgr_start_portal();
      }
      break;

    case WIFI_ST_OK:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[wifi] connection lost, retrying");
        WiFi.reconnect();
        s_connect_started = millis();
        s_state = WIFI_ST_CONNECTING;
      }
      break;

    case WIFI_ST_PORTAL:
      s_dns.processNextRequest();
      s_server.handleClient();
      break;

    case WIFI_ST_OFF:
    default:
      break;
  }
}

WifiState wifi_mgr_state(void) { return s_state; }

String wifi_mgr_ip(void) {
  if (s_state == WIFI_ST_OK) return WiFi.localIP().toString();
  if (s_state == WIFI_ST_PORTAL) return WiFi.softAPIP().toString();
  return String("0.0.0.0");
}

bool wifi_mgr_portal_active(void) { return s_portal_active; }
