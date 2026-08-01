#include <string.h>
#include "auth_cfg.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "auth_cfg";

#define AUTH_CFG_DEFAULT_USERNAME "admin"
#define AUTH_CFG_DEFAULT_PASSWORD "admin"

#define NVS_NAMESPACE    "auth_cfg"
#define NVS_USERNAME_KEY "user"
#define NVS_PASSWORD_KEY "password"

// Reachable from both the httpd worker task (every request, via
// check_admin_auth()) and the serial console task (the "admin" command),
// so the cache is protected by a mutex.
static SemaphoreHandle_t s_mutex;
static char s_username[AUTH_CFG_USERNAME_MAX_LEN];
static char s_password[AUTH_CFG_PASSWORD_MAX_LEN];
static bool s_cache_valid;

esp_err_t auth_cfg_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void auth_cfg_load_from_nvs(char *username, char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);

    if (err == ESP_OK) {
        size_t user_len = AUTH_CFG_USERNAME_MAX_LEN;
        size_t pass_len = AUTH_CFG_PASSWORD_MAX_LEN;

        err = nvs_get_str(nvs_handle, NVS_USERNAME_KEY, username, &user_len);
        if (err != ESP_OK) {
            strcpy(username, AUTH_CFG_DEFAULT_USERNAME);
        }

        err = nvs_get_str(nvs_handle, NVS_PASSWORD_KEY, password, &pass_len);
        if (err != ESP_OK) {
            strcpy(password, AUTH_CFG_DEFAULT_PASSWORD);
        }

        nvs_close(nvs_handle);
    } else {
        strcpy(username, AUTH_CFG_DEFAULT_USERNAME);
        strcpy(password, AUTH_CFG_DEFAULT_PASSWORD);
    }
}

void auth_cfg_load(char *username, char *password)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_cache_valid) {
        auth_cfg_load_from_nvs(s_username, s_password);
        s_cache_valid = true;
    }
    strcpy(username, s_username);
    strcpy(password, s_password);
    xSemaphoreGive(s_mutex);
}

esp_err_t auth_cfg_save(const char *username, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_USERNAME_KEY, username);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_PASSWORD_KEY, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save admin credentials: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved new admin credentials - user: %s", username);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        strcpy(s_username, username);
        strcpy(s_password, password);
        s_cache_valid = true;
        xSemaphoreGive(s_mutex);
    }
    return err;
}

bool auth_cfg_validate(const char *username, const char *password, const char **err_msg)
{
    size_t user_len = strlen(username);
    size_t pass_len = strlen(password);

    if (user_len == 0 || user_len > AUTH_CFG_USERNAME_MAX_LEN - 1) {
        *err_msg = "Admin username must be 1-31 characters";
        return false;
    }
    if (pass_len < 4 || pass_len > AUTH_CFG_PASSWORD_MAX_LEN - 1) {
        *err_msg = "Admin password must be 4-63 characters";
        return false;
    }
    return true;
}
