#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "wan_cfg.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "wan_cfg";

#define NVS_NAMESPACE    "wan_cfg"
#define NVS_ENABLED_KEY  "enabled"
#define NVS_SSID_KEY     "ssid"
#define NVS_PASSWORD_KEY "password"
#define NVS_PORTS_KEY    "ports"

// Reachable from the httpd worker task, the serial console task and wan.c's
// event handlers, so the cache is protected by a mutex - the same arrangement,
// and the same reason, as auth_cfg.c and syslog_cfg.c.
static SemaphoreHandle_t s_mutex;
static wan_cfg_t s_cache;
static bool s_cache_valid;
static uint32_t s_generation;

esp_err_t wan_cfg_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// The compiled-in allowlist. Two jobs are expected of this device in a field,
// and these are their ports:
//
//   2101, 2102        NTRIP. 2101 is what essentially every caster listens on;
//                     2102 is the usual second one.
//   21115/tcp         RustDesk NAT type test.
//   21116/tcp         RustDesk hole punching and connection service.
//   21116/udp         RustDesk ID registration and heartbeat. NOT optional -
//                     without it a client cannot register, and RustDesk falls
//                     back to relay-only if it works at all. This is the reason
//                     the allowlist carries a protocol per rule rather than
//                     being the TCP-only list it started as.
//   21117/tcp         RustDesk relay.
//
// 21118/21119 (RustDesk's web client) are deliberately absent: the desktop
// client does not use them, and this list should not carry doors nobody opens.
static const wan_port_rule_t k_default_ports[] = {
    { 2101,  WAN_PROTO_TCP },
    { 2102,  WAN_PROTO_TCP },
    { 21115, WAN_PROTO_TCP },
    { 21116, WAN_PROTO_TCP },
    { 21116, WAN_PROTO_UDP },
    { 21117, WAN_PROTO_TCP },
};

void wan_cfg_default_ports(wan_port_rule_t *out, uint8_t *out_n)
{
    memcpy(out, k_default_ports, sizeof(k_default_ports));
    *out_n = (uint8_t)(sizeof(k_default_ports) / sizeof(k_default_ports[0]));
}

static void load_from_nvs(wan_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    wan_cfg_default_ports(cfg->ports, &cfg->nports);

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return;
    }

    uint8_t u8 = 0;
    if (nvs_get_u8(nvs_handle, NVS_ENABLED_KEY, &u8) == ESP_OK) {
        cfg->enabled = (u8 != 0);
    }

    size_t len = sizeof(cfg->ssid);
    if (nvs_get_str(nvs_handle, NVS_SSID_KEY, cfg->ssid, &len) != ESP_OK) {
        cfg->ssid[0] = '\0';
    }
    len = sizeof(cfg->password);
    if (nvs_get_str(nvs_handle, NVS_PASSWORD_KEY, cfg->password, &len) != ESP_OK) {
        cfg->password[0] = '\0';
    }

    // The count comes from the blob's length rather than a second key, so there
    // is no way for the two to disagree after a partial write. A blob that is
    // not a whole number of ports, or is longer than the array, is treated as
    // absent and the defaults above stand - the alternative is honouring half of
    // somebody's allowlist, which is the one outcome a policy list must not have.
    size_t blob_len = 0;
    if (nvs_get_blob(nvs_handle, NVS_PORTS_KEY, NULL, &blob_len) == ESP_OK &&
        blob_len > 0 && blob_len <= sizeof(cfg->ports) &&
        blob_len % sizeof(cfg->ports[0]) == 0) {
        wan_port_rule_t tmp[WAN_CFG_MAX_PORTS];
        size_t read_len = blob_len;
        if (nvs_get_blob(nvs_handle, NVS_PORTS_KEY, tmp, &read_len) == ESP_OK &&
            read_len == blob_len) {
            memset(cfg->ports, 0, sizeof(cfg->ports));
            memcpy(cfg->ports, tmp, blob_len);
            cfg->nports = (uint8_t)(blob_len / sizeof(cfg->ports[0]));
        }
    }

    nvs_close(nvs_handle);
}

void wan_cfg_get(wan_cfg_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_cache_valid) {
        load_from_nvs(&s_cache);
        s_cache_valid = true;
    }
    *out = s_cache;
    xSemaphoreGive(s_mutex);
}

uint32_t wan_cfg_generation(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t gen = s_generation;
    xSemaphoreGive(s_mutex);
    return gen;
}

esp_err_t wan_cfg_save(const wan_cfg_t *cfg)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(nvs_handle, NVS_ENABLED_KEY, cfg->enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_SSID_KEY, cfg->ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_PASSWORD_KEY, cfg->password);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs_handle, NVS_PORTS_KEY, cfg->ports,
                           (size_t)cfg->nports * sizeof(cfg->ports[0]));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save WAN config: %s", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cache = *cfg;
    s_cache_valid = true;
    s_generation++;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool wan_cfg_parse_ports(const char *csv, wan_port_rule_t *out, uint8_t *out_n,
                         const char **err_msg)
{
    uint8_t n = 0;
    const char *p = csv;

    // Skip leading whitespace so that a field containing only spaces reads as
    // empty rather than as a malformed entry.
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        *out_n = 0;
        return true;
    }

    for (;;) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p < '0' || *p > '9') {
            *err_msg = "Ports must be numbers separated by commas, like 2101,2102";
            return false;
        }

        // strtoul over atoi so that an overflowing entry is caught here rather
        // than silently wrapping into a port nobody asked to open.
        char *end = NULL;
        unsigned long v = strtoul(p, &end, 10);
        if (v == 0 || v > 65535) {
            *err_msg = "Ports must be 1-65535";
            return false;
        }

        // Optional "/tcp" or "/udp". Bare means TCP.
        uint8_t proto = WAN_PROTO_TCP;
        p = end;
        if (*p == '/') {
            p++;
            if (strncmp(p, "tcp", 3) == 0) {
                proto = WAN_PROTO_TCP;
                p += 3;
            } else if (strncmp(p, "udp", 3) == 0) {
                proto = WAN_PROTO_UDP;
                p += 3;
            } else {
                *err_msg = "After a port, write /tcp or /udp - nothing else";
                return false;
            }
            end = (char *)p;
        }

        if (n >= WAN_CFG_MAX_PORTS) {
            *err_msg = "At most 12 rules can be allowed";
            return false;
        }
        // The pair, not the number: 21116 legitimately appears twice, once per
        // protocol, because RustDesk needs both.
        for (uint8_t i = 0; i < n; i++) {
            if (out[i].port == (uint16_t)v && out[i].proto == proto) {
                *err_msg = "That port and protocol is listed twice";
                return false;
            }
        }
        // Zeroed whole, not field by field: wan_port_rule_t has a padding byte,
        // and wan_cfg_save() writes sizeof(rule) * nports straight into an NVS
        // blob. Leaving the padding as stack residue would put uninitialised
        // bytes in flash and make two identical rule sets compare unequal.
        memset(&out[n], 0, sizeof(out[n]));
        out[n].port = (uint16_t)v;
        out[n].proto = proto;
        n++;

        p = end;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (*p != ',') {
            *err_msg = "Ports must be numbers separated by commas, like 2101,2102";
            return false;
        }
        p++;  // past the comma; the loop head rejects a trailing one
    }

    *out_n = n;
    return true;
}

int wan_cfg_format_ports(const wan_cfg_t *cfg, char *out, size_t outsz)
{
    // Terminate up front. An empty list writes nothing in the loop below, and
    // without this the caller is handed whatever was on its stack - which for
    // wan_get_handler() means uninitialised memory going out in an HTTP
    // response. An allowlist of zero rules is a legitimate state (a disabled
    // uplink saves a half-filled form), so this is reachable, not theoretical.
    if (outsz > 0) {
        out[0] = '\0';
    }

    int len = 0;
    for (uint8_t i = 0; i < cfg->nports; i++) {
        // TCP rules print bare, so a round trip through this and the parser
        // gives back what a person would have typed rather than a normalised
        // form they have to re-read.
        int wrote = snprintf(out + len, (len < (int)outsz) ? outsz - len : 0,
                             "%s%u%s", i ? "," : "", (unsigned)cfg->ports[i].port,
                             cfg->ports[i].proto == WAN_PROTO_UDP ? "/udp" : "");
        if (wrote < 0) {
            break;
        }
        len += wrote;
    }
    if (outsz > 0 && (size_t)len >= outsz) {
        len = (int)outsz - 1;
    }
    return len;
}

bool wan_cfg_validate(const wan_cfg_t *cfg, const char **err_msg)
{
    // Everything below only matters when the WAN will actually run. A
    // disabled configuration with a half-filled form should still save, so that
    // turning it back on later does not mean typing the upstream network's
    // password in again - the same reasoning syslog_cfg_validate() records.
    if (!cfg->enabled) {
        return true;
    }

    size_t ssid_len = strlen(cfg->ssid);
    if (ssid_len == 0 || ssid_len > WAN_CFG_SSID_MAX_LEN - 1) {
        *err_msg = "Upstream network name must be 1-32 characters";
        return false;
    }

    // Blank is allowed and means an open network, unlike this device's own AP
    // (wifi_cfg_validate() always requires a password). The asymmetry is
    // deliberate: refusing to join an open hotspot would not make anything
    // safer, because the egress allowlist - not the upstream's encryption - is
    // what limits what can cross this link.
    size_t pass_len = strlen(cfg->password);
    if (pass_len != 0 && (pass_len < 8 || pass_len > WAN_CFG_PASSWORD_MAX_LEN - 1)) {
        *err_msg = "Upstream password must be 8-63 characters, or blank for an open network";
        return false;
    }

    if (cfg->nports == 0) {
        *err_msg = "Add at least one allowed port - 2101 is the usual NTRIP caster port";
        return false;
    }
    if (cfg->nports > WAN_CFG_MAX_PORTS) {
        *err_msg = "At most 12 rules can be allowed";
        return false;
    }
    for (uint8_t i = 0; i < cfg->nports; i++) {
        if (cfg->ports[i].port == 0) {
            *err_msg = "Ports must be 1-65535";
            return false;
        }
        if (cfg->ports[i].proto != WAN_PROTO_TCP && cfg->ports[i].proto != WAN_PROTO_UDP) {
            *err_msg = "After a port, write /tcp or /udp - nothing else";
            return false;
        }
        for (uint8_t j = 0; j < i; j++) {
            if (cfg->ports[i].port == cfg->ports[j].port &&
                cfg->ports[i].proto == cfg->ports[j].proto) {
                *err_msg = "That port and protocol is listed twice";
                return false;
            }
        }
    }
    return true;
}
