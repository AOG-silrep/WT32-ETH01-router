#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_CFG_SSID_MAX_LEN     32   // includes NUL
#define WIFI_CFG_PASSWORD_MAX_LEN 64   // includes NUL

// Not runtime-configurable via the web UI; shared so main.c's initial
// esp_wifi_set_config() and wifi_cfg_apply()'s later ones stay consistent.
#define WIFI_CFG_CHANNEL        1
#define WIFI_CFG_MAX_STA_CONN   6

// Loads SSID/password from NVS, falling back to compiled-in defaults
// (WIFI_SSID/WIFI_PASS in main.c) if nothing has been saved yet.
// ssid/password buffers must be at least WIFI_CFG_SSID_MAX_LEN/
// WIFI_CFG_PASSWORD_MAX_LEN bytes.
void wifi_cfg_load(char *ssid, char *password);

// Persists SSID/password to NVS. Does not affect the running AP config;
// call wifi_cfg_apply() to make it live.
esp_err_t wifi_cfg_save(const char *ssid, const char *password);

// Applies SSID/password to the running WiFi AP via esp_wifi_set_config().
// The AP restarts immediately: connected WiFi stations briefly drop and
// reconnect under the new SSID; wired/bridge state is unaffected.
esp_err_t wifi_cfg_apply(const char *ssid, const char *password);

// Validates SSID (1-31 bytes) and password (empty for open network, or
// 8-63 bytes for WPA2-PSK). Returns true and leaves *err_msg untouched on
// success; returns false and sets *err_msg to a static, human-readable
// reason on failure.
bool wifi_cfg_validate(const char *ssid, const char *password, const char **err_msg);

#ifdef __cplusplus
}
#endif
