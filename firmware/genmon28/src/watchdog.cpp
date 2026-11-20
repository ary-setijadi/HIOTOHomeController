#include "watchdog.h"

#include "config.h"

#include <esp_system.h>
#include <esp_task_wdt.h>

static bool s_active = false;

void wdt_begin(void) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  /* Arduino core 3.x (IDF 5): configuration is a struct. */
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = (uint32_t)WDT_TIMEOUT_S * 1000U;
  cfg.idle_core_mask = 0; /* watch the loop task only, not the idle tasks */
  cfg.trigger_panic = true;
  esp_err_t err = esp_task_wdt_init(&cfg);
  if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_reconfigure(&cfg);
#else
  /* Arduino core 2.x (IDF 4.4): timeout in seconds + panic flag. If the core
   * already initialised the TWDT (CONFIG_ESP_TASK_WDT_INIT), re-init it so our
   * timeout wins instead of silently keeping the core's default. */
  esp_err_t err = esp_task_wdt_init((uint32_t)WDT_TIMEOUT_S, true);
  if (err == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_deinit();
    err = esp_task_wdt_init((uint32_t)WDT_TIMEOUT_S, true);
  }
#endif

  if (err != ESP_OK) {
    Serial.printf("[wdt] init gagal err=%d - tidak ada watchdog\n", (int)err);
    s_active = false;
    return;
  }

  err = esp_task_wdt_add(NULL); /* subscribe the current (loop) task */
  if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
    Serial.printf("[wdt] add gagal err=%d - tidak ada watchdog\n", (int)err);
    s_active = false;
    return;
  }

  s_active = true;
  esp_task_wdt_reset();
  Serial.printf("[wdt] aktif: timeout %us, panic -> reboot\n", (unsigned)WDT_TIMEOUT_S);
}

void wdt_feed(void) {
  if (s_active) esp_task_wdt_reset();
}

bool wdt_active(void) { return s_active; }

uint32_t wdt_timeout_s(void) { return (uint32_t)WDT_TIMEOUT_S; }

const char *wdt_reset_reason(void) {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software (ESP.restart)";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deepsleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}
