#include <string.h>
#include "wifi_cfg.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_cfg";

#define WIFI_CFG_DEFAULT_SSID "AOG hub"
#define WIFI_CFG_DEFAULT_PASS "password"

#define NVS_NAMESPACE "wifi_config"
#define NVS_SSID_KEY  "ssid"
#define NVS_PASS_KEY  "password"

void wifi_cfg_load(char *ssid, char *password)
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

        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Loaded WiFi config - SSID: %s", ssid);
    } else {
        strcpy(ssid, WIFI_CFG_DEFAULT_SSID);
        strcpy(password, WIFI_CFG_DEFAULT_PASS);
        ESP_LOGI(TAG, "Using default WiFi config");
    }
}

esp_err_t wifi_cfg_save(const char *ssid, const char *password)
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
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save WiFi config: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved WiFi config - SSID: %s", ssid);
    }
    return err;
}

esp_err_t wifi_cfg_apply(const char *ssid, const char *password)
{
    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(ssid),
            .channel = WIFI_CFG_CHANNEL,
            .max_connection = WIFI_CFG_MAX_STA_CONN,
            .authmode = strlen(password) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK,
        },
    };

    strlcpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid));
    strlcpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password));

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Applied WiFi config - SSID: %s", ssid);
    return ESP_OK;
}

bool wifi_cfg_validate(const char *ssid, const char *password, const char **err_msg)
{
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(password);

    if (ssid_len == 0 || ssid_len > WIFI_CFG_SSID_MAX_LEN - 1) {
        *err_msg = "SSID must be 1-31 characters";
        return false;
    }
    if (pass_len != 0 && (pass_len < 8 || pass_len > WIFI_CFG_PASSWORD_MAX_LEN - 1)) {
        *err_msg = "Password must be empty (open network) or 8-63 characters";
        return false;
    }
    return true;
}
