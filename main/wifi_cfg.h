#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_CFG_SSID_MAX_LEN     32   // includes NUL
#define WIFI_CFG_PASSWORD_MAX_LEN 64   // includes NUL

#define WIFI_CFG_DEFAULT_CHANNEL 1
#define WIFI_CFG_MAX_STA_CONN    6

// Loads SSID/password/channel from NVS, falling back to compiled-in
// defaults if nothing has been saved yet. ssid/password buffers must be at
// least WIFI_CFG_SSID_MAX_LEN/WIFI_CFG_PASSWORD_MAX_LEN bytes.
void wifi_cfg_load(char *ssid, char *password, uint8_t *channel);

// Persists SSID/password/channel to NVS. Does not affect the running AP
// config; call wifi_cfg_apply() to make it live.
esp_err_t wifi_cfg_save(const char *ssid, const char *password, uint8_t channel);

// Applies SSID/password/channel to the running WiFi AP via
// esp_wifi_set_config(). The AP restarts immediately: connected WiFi
// stations briefly drop and reconnect under the new config; wired/bridge
// state is unaffected.
esp_err_t wifi_cfg_apply(const char *ssid, const char *password, uint8_t channel);

// Validates SSID (1-31 bytes), password (empty for open network, or 8-63
// bytes for WPA2-PSK), and channel (must be 1, 6, or 11 - the non-overlapping
// 2.4GHz channels). Returns true and leaves *err_msg untouched on success;
// returns false and sets *err_msg to a static, human-readable reason on
// failure.
bool wifi_cfg_validate(const char *ssid, const char *password, uint8_t channel, const char **err_msg);

#ifdef __cplusplus
}
#endif
