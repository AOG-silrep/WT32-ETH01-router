#include <string.h>
#include <stdio.h>
#include "syslog_cfg.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "syslog_cfg";

#define NVS_NAMESPACE     "syslog_cfg"
#define NVS_ENABLED_KEY   "enabled"
#define NVS_IP_KEY        "ip"
#define NVS_PORT_KEY      "port"
#define NVS_FACILITY_KEY  "fac"
#define NVS_SEVERITY_KEY  "sev"
#define NVS_HOSTNAME_KEY  "host"

// Reachable from the httpd worker task, the serial console task and the syslog
// sender task, so the cache is protected by a mutex - the same arrangement, and
// the same reason, as auth_cfg.c.
static SemaphoreHandle_t s_mutex;
static syslog_cfg_t s_cache;
static bool s_cache_valid;
static uint32_t s_generation;

esp_err_t syslog_cfg_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void syslog_cfg_default_hostname(char *out, size_t outsz)
{
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_ETH) != ESP_OK) {
        strlcpy(out, "wt32-bridge", outsz);
        return;
    }
    // A MAC-derived name rather than a fixed string, because a collector
    // receiving from two of these devices has nothing else to tell them apart:
    // TIMESTAMP is the NILVALUE, PROCID restarts at every boot, and the source
    // address is whichever one this bridge holds. The name is the only field
    // that identifies the device.
    //
    // ESP_MAC_ETH is the MAC main.c gives both bridge ports and the bridge
    // itself, so the name matches what the network sees. Lower-case hex with no
    // separators: HOSTNAME is PRINTUSASCII, and a colon there reads as a field
    // separator to more than one collector.
    snprintf(out, outsz, "wt32-bridge-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static void load_from_nvs(syslog_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = SYSLOG_CFG_DEFAULT_PORT;
    cfg->facility = SYSLOG_CFG_DEFAULT_FACILITY;
    cfg->min_severity = SYSLOG_CFG_DEFAULT_SEVERITY;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        uint8_t u8 = 0;
        if (nvs_get_u8(nvs_handle, NVS_ENABLED_KEY, &u8) == ESP_OK) {
            cfg->enabled = (u8 != 0);
        }
        nvs_get_u32(nvs_handle, NVS_IP_KEY, &cfg->server_ip);
        nvs_get_u16(nvs_handle, NVS_PORT_KEY, &cfg->port);
        nvs_get_u8(nvs_handle, NVS_FACILITY_KEY, &cfg->facility);
        nvs_get_u8(nvs_handle, NVS_SEVERITY_KEY, &cfg->min_severity);

        size_t host_len = sizeof(cfg->hostname);
        if (nvs_get_str(nvs_handle, NVS_HOSTNAME_KEY, cfg->hostname, &host_len) != ESP_OK) {
            cfg->hostname[0] = '\0';
        }
        nvs_close(nvs_handle);
    }

    // An empty stored name means "whatever the default is", resolved here so
    // that "unset" never reaches a caller as a name it has to special-case -
    // and so that a device whose name was never set still identifies itself.
    if (cfg->hostname[0] == '\0') {
        syslog_cfg_default_hostname(cfg->hostname, sizeof(cfg->hostname));
    }
}

void syslog_cfg_get(syslog_cfg_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_cache_valid) {
        load_from_nvs(&s_cache);
        s_cache_valid = true;
    }
    *out = s_cache;
    xSemaphoreGive(s_mutex);
}

uint32_t syslog_cfg_generation(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t gen = s_generation;
    xSemaphoreGive(s_mutex);
    return gen;
}

esp_err_t syslog_cfg_save(const syslog_cfg_t *cfg)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    // Stored as written, including an empty hostname if that is what was asked
    // for: the resolution to the MAC-derived default happens on load, so a
    // device that is renamed by a firmware change picks the new default up
    // rather than being stuck with the one baked in when it was first saved.
    err = nvs_set_u8(nvs_handle, NVS_ENABLED_KEY, cfg->enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs_handle, NVS_IP_KEY, cfg->server_ip);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs_handle, NVS_PORT_KEY, cfg->port);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, NVS_FACILITY_KEY, cfg->facility);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, NVS_SEVERITY_KEY, cfg->min_severity);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_HOSTNAME_KEY, cfg->hostname);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save syslog config: %s", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cache = *cfg;
    if (s_cache.hostname[0] == '\0') {
        syslog_cfg_default_hostname(s_cache.hostname, sizeof(s_cache.hostname));
    }
    s_cache_valid = true;
    s_generation++;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool syslog_cfg_validate(const syslog_cfg_t *cfg, uint32_t subnet_ip,
                         uint32_t subnet_mask, const char **err_msg)
{
    // Everything below only matters when the sender will actually run. A
    // disabled configuration with a half-filled form should still save, so that
    // turning it back on later does not mean typing the collector's address in
    // again.
    if (!cfg->enabled) {
        return true;
    }

    if (cfg->server_ip == 0) {
        *err_msg = "Enter the syslog collector's IP address";
        return false;
    }
    if (subnet_mask == 0) {
        // Fail closed rather than skip the check. An unchecked address that
        // turns out to be off-subnet is a sender that silently sends nowhere,
        // which is precisely the failure the check below exists to make
        // visible - so not being able to run it is a reason to refuse, not a
        // reason to wave it through.
        *err_msg = "Could not read this bridge's own address, so the collector's could not be checked";
        return false;
    }
    // The bridge has one interface and no route off it: main.c sets the bridge
    // netif's gateway to the bridge's own address. A collector anywhere else is
    // not merely unreachable, it is unreachable in a way nothing reports -
    // sendto() to an off-subnet address on a socket pinned to the bridge fails
    // with ENETUNREACH if it fails at all, and what the person who typed it
    // sees is a device claiming to ship logs and a collector that never
    // receives one.
    //
    // The message names the subnet literally, duplicating BRIDGE_IP/
    // BRIDGE_NETMASK in main.c:38-39. Formatting the real one in would cost a
    // non-static err_msg and break the pattern every other *_validate() in this
    // repo follows; if the addressing ever changes, this string is the second
    // place to look.
    if ((cfg->server_ip & subnet_mask) != (subnet_ip & subnet_mask)) {
        *err_msg = "The collector must be on this bridge's own 192.168.5.0/24 network - "
                   "the bridge has no gateway and cannot reach anything off it";
        return false;
    }
    if (cfg->server_ip == subnet_ip) {
        *err_msg = "That is this device's own address - enter the collector's";
        return false;
    }
    // SO_BROADCAST is deliberately not set on the sender's socket, so a
    // broadcast address here would fail at every single sendto() with EACCES.
    // Rejecting it by name says why; letting it through would produce a
    // send_fail counter climbing once per log line and no other explanation.
    if ((cfg->server_ip | ~subnet_mask) == cfg->server_ip) {
        *err_msg = "Enter one collector's address, not the broadcast address";
        return false;
    }
    if (cfg->port == 0) {
        *err_msg = "Port must be 1-65535 (514 is the syslog default)";
        return false;
    }
    if (cfg->facility > 23) {
        *err_msg = "Facility must be 0-23 (23 is local7)";
        return false;
    }
    if (cfg->min_severity > 7) {
        *err_msg = "Minimum severity must be 0-7";
        return false;
    }

    // HOSTNAME is PRINTUSASCII, 1-255 characters (RFC 5424 section 6.2.4).
    // Rejected here rather than sanitised in the sender: a name silently
    // rewritten is a name nobody can find on the collector, and the person who
    // typed it has no way to learn what it became.
    size_t host_len = strlen(cfg->hostname);
    if (host_len == 0 || host_len > SYSLOG_CFG_HOSTNAME_MAX_LEN - 1) {
        *err_msg = "Hostname must be 1-32 characters";
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)cfg->hostname; *p != '\0'; p++) {
        if (*p < 33 || *p > 126) {
            *err_msg = "Hostname must be printable ASCII with no spaces";
            return false;
        }
    }
    return true;
}

// The facilities worth naming. Everything else formats as a number rather than
// being rejected: the field is 0-23 by the RFC, and a collector configured to
// sort on an unusual one is not this device's business to second-guess.
static const char *const k_facility_names[] = {
    "kernel", "user", "mail", "daemon", "auth", "syslog", "lpr", "news",
    "uucp", "cron", "authpriv", "ftp", "ntp", "audit", "alert", "clock",
    "local0", "local1", "local2", "local3", "local4", "local5", "local6", "local7",
};

const char *syslog_cfg_facility_name(uint8_t facility)
{
    if (facility < sizeof(k_facility_names) / sizeof(k_facility_names[0])) {
        return k_facility_names[facility];
    }
    return "unknown";
}

// RFC 5424 section 6.2.1 severities. The four this firmware can actually
// produce ("error", "warning", "info", "debug") deliberately carry the same
// names the "loglevel" command and the log page's capture select use, so the
// three interfaces do not end up with three vocabularies for one idea.
static const char *const k_severity_names[] = {
    "emergency", "alert", "critical", "error",
    "warning", "notice", "info", "debug",
};

const char *syslog_cfg_severity_name(uint8_t severity)
{
    if (severity < sizeof(k_severity_names) / sizeof(k_severity_names[0])) {
        return k_severity_names[severity];
    }
    return NULL;
}

bool syslog_cfg_severity_from_name(const char *name, uint8_t *out)
{
    for (size_t i = 0; i < sizeof(k_severity_names) / sizeof(k_severity_names[0]); i++) {
        if (strcmp(name, k_severity_names[i]) == 0) {
            *out = (uint8_t)i;
            return true;
        }
    }
    return false;
}
