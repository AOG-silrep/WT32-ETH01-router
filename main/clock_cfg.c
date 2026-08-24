#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>
#include "clock_cfg.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "clock_cfg";

#define NVS_NAMESPACE "clock_cfg"
#define NVS_TZ_KEY    "tz"

// Reachable from the httpd worker task, the serial console task and the SNTP
// sync callback, so the cache is protected by a mutex - the same arrangement,
// and the same reason, as syslog_cfg.c.
static SemaphoreHandle_t s_mutex;
static clock_cfg_t s_cache;
static bool s_cache_valid;

// Labels are IANA names because that is what a person recognises, but what is
// stored and applied is the POSIX string beside it - newlib has no zoneinfo
// database to look an IANA name up in.
//
// North America first and in full, because that is where these devices are.
// The rest are the zones a machine has actually turned up in, not a survey.
// Anyone outside the list can paste their own POSIX string, which is why the
// form keeps a text field beside the menu.
static const struct {
    const char *label;
    const char *tz;
} k_zones[] = {
    { "UTC",                        "UTC0"                          },
    { "America/St_Johns",           "NST3:30NDT,M3.2.0,M11.1.0"     },
    { "America/Halifax",            "AST4ADT,M3.2.0,M11.1.0"        },
    { "America/New_York",           "EST5EDT,M3.2.0,M11.1.0"        },
    { "America/Chicago",            "CST6CDT,M3.2.0,M11.1.0"        },
    { "America/Denver",             "MST7MDT,M3.2.0,M11.1.0"        },
    { "America/Phoenix",            "MST7"                          },
    { "America/Los_Angeles",        "PST8PDT,M3.2.0,M11.1.0"        },
    { "America/Anchorage",          "AKST9AKDT,M3.2.0,M11.1.0"      },
    { "Pacific/Honolulu",           "HST10"                         },
    { "America/Regina",             "CST6"                          },
    { "America/Mexico_City",        "CST6"                          },
    { "America/Sao_Paulo",          "<-03>3"                        },
    { "Europe/London",              "GMT0BST,M3.5.0/1,M10.5.0"      },
    { "Europe/Dublin",              "IST-1GMT0,M10.5.0,M3.5.0/1"    },
    { "Europe/Paris",               "CET-1CEST,M3.5.0,M10.5.0/3"    },
    { "Europe/Berlin",              "CET-1CEST,M3.5.0,M10.5.0/3"    },
    { "Europe/Kyiv",                "EET-2EEST,M3.5.0/3,M10.5.0/4"  },
    { "Europe/Moscow",              "MSK-3"                         },
    { "Africa/Johannesburg",        "SAST-2"                        },
    { "Asia/Jerusalem",             "IST-2IDT,M3.4.4/26,M10.5.0"    },
    { "Asia/Kolkata",               "IST-5:30"                      },
    { "Asia/Shanghai",              "CST-8"                         },
    { "Asia/Tokyo",                 "JST-9"                         },
    { "Australia/Perth",            "AWST-8"                        },
    { "Australia/Adelaide",         "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    { "Australia/Sydney",           "AEST-10AEDT,M10.1.0,M4.1.0/3"  },
    { "Pacific/Auckland",           "NZST-12NZDT,M9.5.0,M4.1.0/3"   },
};

#define ZONE_COUNT (sizeof(k_zones) / sizeof(k_zones[0]))

bool clock_cfg_zone(int i, const char **label, const char **tz)
{
    if (i < 0 || (size_t)i >= ZONE_COUNT) {
        return false;
    }
    *label = k_zones[i].label;
    *tz = k_zones[i].tz;
    return true;
}

const char *clock_cfg_zone_from_label(const char *label)
{
    if (label == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < ZONE_COUNT; i++) {
        if (strcasecmp(label, k_zones[i].label) == 0) {
            return k_zones[i].tz;
        }
    }
    return NULL;
}

// Applied to the C library rather than kept only in the cache: every timestamp
// this device renders goes through localtime_r(), which reads the environment
// and nothing else.
static void apply(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

static void load_from_nvs(clock_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(cfg->tz);
        if (nvs_get_str(h, NVS_TZ_KEY, cfg->tz, &len) != ESP_OK) {
            cfg->tz[0] = '\0';
        }
        nvs_close(h);
    }

    // An empty stored string means "whatever the default is", resolved here so
    // "unset" never reaches a caller as a value it has to special-case.
    if (cfg->tz[0] == '\0') {
        strlcpy(cfg->tz, CLOCK_CFG_TZ_DEFAULT, sizeof(cfg->tz));
    }
}

esp_err_t clock_cfg_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // Loaded and applied here rather than lazily on first get(), so the zone is
    // in force before anything can render a time. reset_log.c stamps a record
    // during startup on the RTC-survived path, and a record rendered in UTC
    // because the cache had not been touched yet would be wrong by hours with
    // nothing on screen to say so.
    clock_cfg_t cfg;
    load_from_nvs(&cfg);
    apply(cfg.tz);

    s_cache = cfg;
    s_cache_valid = true;
    return ESP_OK;
}

void clock_cfg_get(clock_cfg_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_cache_valid) {
        load_from_nvs(&s_cache);
        s_cache_valid = true;
    }
    *out = s_cache;
    xSemaphoreGive(s_mutex);
}

esp_err_t clock_cfg_save(const clock_cfg_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, NVS_TZ_KEY, cfg->tz);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save timezone: %s", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cache = *cfg;
    if (s_cache.tz[0] == '\0') {
        strlcpy(s_cache.tz, CLOCK_CFG_TZ_DEFAULT, sizeof(s_cache.tz));
    }
    s_cache_valid = true;
    apply(s_cache.tz);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "timezone set to %s", s_cache.tz);
    return ESP_OK;
}

bool clock_cfg_validate(const clock_cfg_t *cfg, const char **err_msg)
{
    size_t len = strnlen(cfg->tz, sizeof(cfg->tz));
    if (len == sizeof(cfg->tz)) {
        *err_msg = "Timezone is too long";
        return false;
    }
    // An empty string is accepted and resolves to the default on save, so
    // clearing the field is how a person gets back to UTC.
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)cfg->tz[i];
        // setenv() takes anything, but a control character here would reach the
        // console's own output and the JSON, and neither has a use for one.
        if (c < 0x20 || c > 0x7e) {
            *err_msg = "Timezone must be plain ASCII, e.g. CST6CDT,M3.2.0,M11.1.0";
            return false;
        }
    }
    return true;
}
