#include <string.h>
#include "wifi_cfg.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_cfg";

#define WIFI_CFG_DEFAULT_SSID "AOG hub"
#define WIFI_CFG_DEFAULT_PASS "password"

#define NVS_NAMESPACE    "wifi_config"
#define NVS_SSID_KEY     "ssid"
#define NVS_PASS_KEY     "password"
#define NVS_CHANNEL_KEY  "channel"

void wifi_cfg_load(char *ssid, char *password, uint8_t *channel)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);

    if (err == ESP_OK) {
        size_t ssid_len = WIFI_CFG_SSID_MAX_LEN;
        size_t pass_len = WIFI_CFG_PASSWORD_MAX_LEN;

        err = nvs_get_str(nvs_handle, NVS_SSID_KEY, ssid, &ssid_len);
        if (err != ESP_OK) {
            strcpy(ssid, WIFI_CFG_DEFAULT_SSID);
        }

        err = nvs_get_str(nvs_handle, NVS_PASS_KEY, password, &pass_len);
        if (err != ESP_OK) {
            strcpy(password, WIFI_CFG_DEFAULT_PASS);
        }

        uint8_t stored_channel = 0;
        err = nvs_get_u8(nvs_handle, NVS_CHANNEL_KEY, &stored_channel);
        *channel = (err == ESP_OK) ? stored_channel : WIFI_CFG_DEFAULT_CHANNEL;

        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Loaded WiFi config - SSID: %s, channel: %u", ssid, *channel);
    } else {
        strcpy(ssid, WIFI_CFG_DEFAULT_SSID);
        strcpy(password, WIFI_CFG_DEFAULT_PASS);
        *channel = WIFI_CFG_DEFAULT_CHANNEL;
        ESP_LOGI(TAG, "Using default WiFi config");
    }
}

esp_err_t wifi_cfg_save(const char *ssid, const char *password, uint8_t channel)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_SSID_KEY, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_PASS_KEY, password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, NVS_CHANNEL_KEY, channel);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save WiFi config: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved WiFi config - SSID: %s, channel: %u", ssid, channel);
    }
    return err;
}

esp_err_t wifi_cfg_apply(const char *ssid, const char *password, uint8_t channel)
{
    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(ssid),
            .channel = channel,
            .max_connection = WIFI_CFG_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    strlcpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid));
    strlcpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password));

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    // Turn off Protected Management Frames, which the AP would otherwise
    // advertise whether or not this file asks for it: pmf_cfg.capable is
    // documented as "deprecated and set to true internally", so leaving the
    // field alone does not leave PMF alone. This call is the only way off, and
    // it has to sit here - the API requires it after esp_wifi_set_config() and
    // before esp_wifi_start(), which is exactly this window.
    //
    // It is disabled because the SA Query procedure PMF brings with it does not
    // survive a station re-associating with this AP. The device's own log,
    // during a plain TCP download to a WiFi client:
    //
    //   wifi: starting SA query procedure with STA(fc:e8:c0:4d:ab:94)
    //   wifi: Send SA Query req with transaction id ...   (six of these)
    //   wifi: STA not responded to 6 SA Query attempts, Reset connection ...
    //   wifi: station: fc:e8:c0:4d:ab:94 leave, AID = 2, reason = 209
    //   wifi: station: fc:e8:c0:4d:ab:94 join, AID = 2, bgn, 40U
    //
    // and then straight back to the top, every few seconds, indefinitely. Each
    // cycle throws away everything queued for that station, so TCP loses its
    // congestion window and never gets it back before the next disassociation:
    // measured downlink was 1.14 Mbit/s against 27.0 in the other direction,
    // with a third of all bytes retransmitted. UDP looked healthier only
    // because it does not care - the same disconnects were there, showing up as
    // a gap rather than as a collapse.
    //
    // What this gives up is PMF's protection against forged deauthentication
    // and disassociation frames, for the stations that support it. That is a
    // real loss, and on a link that stayed up it would not be worth making. It
    // is worth making here: an access point that ejects its clients every few
    // seconds is not delivering the security either, and this bridge exists to
    // carry traffic. The WPA2-PSK encryption of actual data is untouched.
    err = esp_wifi_disable_pmf_config(WIFI_IF_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_disable_pmf_config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Applied WiFi config - SSID: %s, channel: %u (PMF off)", ssid, channel);
    return ESP_OK;
}

bool wifi_cfg_validate(const char *ssid, const char *password, uint8_t channel, const char **err_msg)
{
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(password);

    if (ssid_len == 0 || ssid_len > WIFI_CFG_SSID_MAX_LEN - 1) {
        *err_msg = "SSID must be 1-31 characters";
        return false;
    }
    if (pass_len < 8 || pass_len > WIFI_CFG_PASSWORD_MAX_LEN - 1) {
        *err_msg = "Password must be 8-63 characters";
        return false;
    }
    if (channel != 1 && channel != 6 && channel != 11) {
        *err_msg = "Channel must be 1, 6, or 11";
        return false;
    }
    return true;
}
