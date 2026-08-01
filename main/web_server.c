#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "web_server.h"
#include "wifi_cfg.h"
#include "auth_cfg.h"
#include "client_track.h"
#include "sys_monitor.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web_server";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t admin_html_start[] asm("_binary_admin_html_start");
extern const uint8_t admin_html_end[] asm("_binary_admin_html_end");

// Pulls a JSON string field's value out of a flat {"key":"value",...}
// object. Good enough for the small, fixed-shape request bodies this
// server accepts - not a general JSON parser.
static bool json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return false;
    }
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t i = 0;
    while (*p != '\0' && *p != '"' && i < out_len - 1) {
        if (*p == '\\' && *(p + 1) != '\0') {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

// Pulls a JSON numeric field's value out of a flat {"key":123,...} object.
static bool json_get_int(const char *json, const char *key, long *out)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return false;
    }
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) {
        return false;
    }
    p++;
    char *end;
    long val = strtol(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = val;
    return true;
}

static int base64_decode_char(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decodes a standard base64 string (RFC 4648, '+'/'/' alphabet, '='
// padding) - just enough to unpack the "Authorization: Basic ..." header,
// not a general-purpose decoder.
static bool base64_decode(const char *in, size_t in_len, char *out, size_t out_cap, size_t *out_len)
{
    if (in_len == 0 || in_len % 4 != 0) {
        return false;
    }
    size_t written = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        int vals[4];
        int pad = 0;
        for (int j = 0; j < 4; j++) {
            char c = in[i + j];
            if (c == '=') {
                vals[j] = 0;
                pad++;
            } else {
                vals[j] = base64_decode_char(c);
                if (vals[j] < 0) {
                    return false;
                }
            }
        }
        unsigned char b0 = (unsigned char)((vals[0] << 2) | (vals[1] >> 4));
        unsigned char b1 = (unsigned char)((vals[1] << 4) | (vals[2] >> 2));
        unsigned char b2 = (unsigned char)((vals[2] << 6) | vals[3]);

        int out_bytes = 3 - pad;
        if (out_bytes >= 1) {
            if (written >= out_cap) return false;
            out[written++] = (char)b0;
        }
        if (out_bytes >= 2) {
            if (written >= out_cap) return false;
            out[written++] = (char)b1;
        }
        if (out_bytes >= 3) {
            if (written >= out_cap) return false;
            out[written++] = (char)b2;
        }
    }
    *out_len = written;
    return true;
}

// Checks the "Authorization: Basic ..." header against the saved admin
// credentials. Using standard HTTP Basic Auth (rather than a bespoke
// header/cookie scheme) means the browser itself prompts for and caches
// the credentials, so every route - including the page itself - can be
// gated with no client-side login code: the very first request for "/"
// gets a 401 challenge, the browser's native dialog collects
// username/password, and every request after that (including the JS
// polling fetch() calls) automatically carries the Authorization header.
static bool check_admin_auth(httpd_req_t *req)
{
    char hdr[160]; // "Basic " + base64(longest possible "user:password")
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len == 0 || hdr_len >= sizeof(hdr)) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }

    const char *prefix = "Basic ";
    size_t prefix_len = strlen(prefix);
    if (strncmp(hdr, prefix, prefix_len) != 0) {
        return false;
    }

    char decoded[AUTH_CFG_USERNAME_MAX_LEN + AUTH_CFG_PASSWORD_MAX_LEN];
    size_t decoded_len = 0;
    if (!base64_decode(hdr + prefix_len, hdr_len - prefix_len, decoded, sizeof(decoded) - 1, &decoded_len)) {
        return false;
    }
    decoded[decoded_len] = '\0';

    char *colon = strchr(decoded, ':');
    if (colon == NULL) {
        return false;
    }
    *colon = '\0';
    const char *user = decoded;
    const char *password = colon + 1;

    char expected_user[AUTH_CFG_USERNAME_MAX_LEN];
    char expected_password[AUTH_CFG_PASSWORD_MAX_LEN];
    auth_cfg_load(expected_user, expected_password);

    if (strcmp(user, expected_user) != 0) {
        return false;
    }
    return strcmp(password, expected_password) == 0;
}

static void send_auth_error(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"AgOpen router\"");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"Authentication required\"}", HTTPD_RESP_USE_STRLEN);
}

static void send_default_creds_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/admin?forced=1");
    httpd_resp_send(req, NULL, 0);
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_is_default()) {
        send_default_creds_redirect(req);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                     index_html_end - index_html_start - 1);
    return ESP_OK;
}

static esp_err_t admin_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)admin_html_start,
                     admin_html_end - admin_html_start - 1);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }

    wifi_config_t cfg;
    esp_wifi_get_config(WIFI_IF_AP, &cfg);

    char resp[80];
    int len = snprintf(resp, sizeof(resp), "{\"ssid\":\"%s\",\"channel\":%u}",
                        (const char *)cfg.ap.ssid, (unsigned)cfg.ap.channel);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }

    char buf[256];
    if (req->content_len <= 0 || req->content_len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body missing or too large");
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char ssid[WIFI_CFG_SSID_MAX_LEN] = {0};
    char password[WIFI_CFG_PASSWORD_MAX_LEN] = {0};
    if (!json_get_string(buf, "ssid", ssid, sizeof(ssid))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing \"ssid\"");
        return ESP_FAIL;
    }
    json_get_string(buf, "password", password, sizeof(password)); // absent/blank -> keep current password
    if (password[0] == '\0') {
        char cur_ssid[WIFI_CFG_SSID_MAX_LEN];
        uint8_t cur_channel;
        wifi_cfg_load(cur_ssid, password, &cur_channel);
    }

    long channel_val = WIFI_CFG_DEFAULT_CHANNEL;
    json_get_int(buf, "channel", &channel_val); // absent -> default channel
    uint8_t channel = (channel_val < 0 || channel_val > 255) ? 0 : (uint8_t)channel_val;

    const char *err_msg = NULL;
    if (!wifi_cfg_validate(ssid, password, channel, &err_msg)) {
        char resp[128];
        int len = snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", err_msg);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, len);
        return ESP_OK;
    }

    if (wifi_cfg_save(ssid, password, channel) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Failed to save\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);

    // Reboot rather than reconfiguring the AP in place: the WiFi AP netif is
    // a bridge port wired into esp_netif_br_glue, and hot-applying a new
    // config via esp_wifi_set_config() leaves the bridge broken until a
    // manual power-cycle. A full restart re-applies the saved config
    // cleanly via the normal boot path (wifi_init_softap()). Delay briefly
    // so the response above actually reaches the client first.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t admin_post_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }

    char buf[256];
    if (req->content_len <= 0 || req->content_len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body missing or too large");
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char new_user[AUTH_CFG_USERNAME_MAX_LEN];
    char new_password[AUTH_CFG_PASSWORD_MAX_LEN];
    auth_cfg_load(new_user, new_password); // start from current values - either field may be left unchanged

    char tmp_user[AUTH_CFG_USERNAME_MAX_LEN] = {0};
    char tmp_password[AUTH_CFG_PASSWORD_MAX_LEN] = {0};
    if (json_get_string(buf, "new_admin_user", tmp_user, sizeof(tmp_user)) && tmp_user[0] != '\0') {
        strcpy(new_user, tmp_user);
    }
    if (json_get_string(buf, "new_admin_password", tmp_password, sizeof(tmp_password)) && tmp_password[0] != '\0') {
        strcpy(new_password, tmp_password);
    }

    const char *err_msg = NULL;
    if (!auth_cfg_validate(new_user, new_password, &err_msg)) {
        char resp[128];
        int len = snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", err_msg);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, len);
        return ESP_OK;
    }

    if (auth_cfg_save(new_user, new_password) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Failed to save\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // No reboot needed - check_admin_auth() reloads credentials from NVS on
    // every request, so the new pair takes effect on the very next one. The
    // browser's cached Basic Auth credentials for this session are now
    // stale and it'll be re-challenged next time it hits any endpoint.
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    // Static, not stack-allocated: the httpd worker task's stack is shared
    // across all handlers, and a 4KB local buffer here would blow it.
    static uint8_t ota_buf[4096];

    if (!check_admin_auth(req)) {
        send_auth_error(req);
        // Drain the body ourselves in large chunks. Left to the framework,
        // the post-handler purge reads CONFIG_HTTPD_PURGE_BUF_LEN (32)
        // bytes at a time against a multi-megabyte image; any inter-chunk
        // gap past recv_wait_timeout (5s) makes it bail and force-close the
        // socket mid-upload, which surfaces to the client as a connection
        // reset instead of the 401 it already received.
        int remaining = req->content_len;
        while (remaining > 0) {
            int to_read = remaining < sizeof(ota_buf) ? remaining : sizeof(ota_buf);
            int received = httpd_req_recv(req, (char *)ota_buf, to_read);
            if (received <= 0) {
                break;
            }
            remaining -= received;
        }
        return ESP_OK;
    }

    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body missing");
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition available");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, req->content_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Image too large or OTA busy");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining < sizeof(ota_buf) ? remaining : sizeof(ota_buf);
        int received = httpd_req_recv(req, (char *)ota_buf, to_read);
        if (received <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, ota_buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
            return ESP_FAIL;
        }
        remaining -= received;
    }

    // esp_ota_end() validates the image header/checksum - on failure the
    // partition just written is left un-booted, so a corrupt upload can't
    // brick the device; it just doesn't take effect.
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        char resp[128];
        int len = snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"Image validation failed: %s\"}",
                            esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, len);
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Failed to set boot partition\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);

    // Same pattern as wifi_post_handler: let the response reach the client
    // before tearing everything down for the reboot into the new image.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t clients_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }

    client_info_t clients[CLIENT_TRACK_MAX_CLIENTS];
    int count = 0;
    client_track_get_snapshot(clients, CLIENT_TRACK_MAX_CLIENTS, &count);

    static char resp[CLIENT_TRACK_MAX_CLIENTS * (200 + CLIENT_TRACK_NAME_MAX_LEN) + 16];
    int off = snprintf(resp, sizeof(resp), "[");
    for (int i = 0; i < count && off < sizeof(resp); i++) {
        client_info_t *c = &clients[i];
        char ip_str[16];
        esp_ip4addr_ntoa(&c->ip, ip_str, sizeof(ip_str));
        char rssi_str[8];
        if (c->is_wifi) {
            snprintf(rssi_str, sizeof(rssi_str), "%d", c->rssi);
        } else {
            strcpy(rssi_str, "null");
        }
        off += snprintf(resp + off, sizeof(resp) - off,
                         "%s{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ip\":\"%s\","
                         "\"link\":\"%s\",\"rssi\":%s,\"rx_bps\":%u,\"tx_bps\":%u,"
                         "\"rx_pps\":%u,\"tx_pps\":%u,"
                         "\"last_seen_s\":%u,\"name\":\"%s\"}",
                         i == 0 ? "" : ",",
                         c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5],
                         ip_str, c->is_wifi ? "wifi" : "eth", rssi_str,
                         (unsigned)c->rx_bps, (unsigned)c->tx_bps,
                         (unsigned)c->rx_pps, (unsigned)c->tx_pps, (unsigned)c->last_seen_s,
                         c->name);
    }
    off += snprintf(resp + off, sizeof(resp) - off, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, off);
    return ESP_OK;
}

// Parses "aa:bb:cc:dd:ee:ff" into 6 raw bytes. Uses an unsigned int scratch
// array rather than scanning directly into mac's uint8_t slots - %hhx support
// varies across newlib scanf configurations, %x into a wider int does not.
static bool parse_mac(const char *str, uint8_t mac[6])
{
    unsigned int b[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)b[i];
    }
    return true;
}

static esp_err_t client_history_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }

    char query[128];
    char mac_str[24] = {0};
    char since_str[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "mac", mac_str, sizeof(mac_str));
        httpd_query_key_value(query, "since", since_str, sizeof(since_str));
    }

    uint8_t mac[6];
    if (!parse_mac(mac_str, mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid \"mac\"");
        return ESP_FAIL;
    }
    uint32_t since_seq = since_str[0] ? (uint32_t)strtoul(since_str, NULL, 10) : 0;

    client_history_t hist;
    if (!client_track_get_history(mac, since_seq, &hist)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown client");
        return ESP_FAIL;
    }

    static char resp[1600];
    int off = snprintf(resp, sizeof(resp),
                        "{\"mac\":\"%s\",\"period_ms\":%d,\"seq\":%u,\"reset\":%s,\"rx\":[",
                        mac_str, CLIENT_TRACK_HISTORY_PERIOD_MS, (unsigned)hist.seq,
                        hist.reset ? "true" : "false");
    for (int i = 0; i < hist.count && off < sizeof(resp); i++) {
        off += snprintf(resp + off, sizeof(resp) - off, "%s%u", i == 0 ? "" : ",", (unsigned)hist.rx_bps[i]);
    }
    off += snprintf(resp + off, sizeof(resp) - off, "],\"tx\":[");
    for (int i = 0; i < hist.count && off < sizeof(resp); i++) {
        off += snprintf(resp + off, sizeof(resp) - off, "%s%u", i == 0 ? "" : ",", (unsigned)hist.tx_bps[i]);
    }
    off += snprintf(resp + off, sizeof(resp) - off, "],\"rx_pps\":[");
    for (int i = 0; i < hist.count && off < sizeof(resp); i++) {
        off += snprintf(resp + off, sizeof(resp) - off, "%s%u", i == 0 ? "" : ",", (unsigned)hist.rx_pps[i]);
    }
    off += snprintf(resp + off, sizeof(resp) - off, "],\"tx_pps\":[");
    for (int i = 0; i < hist.count && off < sizeof(resp); i++) {
        off += snprintf(resp + off, sizeof(resp) - off, "%s%u", i == 0 ? "" : ",", (unsigned)hist.tx_pps[i]);
    }
    off += snprintf(resp + off, sizeof(resp) - off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, off);
    return ESP_OK;
}

static esp_err_t system_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }

    uint8_t cpu_pct[SYS_MONITOR_NUM_CORES];
    sys_monitor_get_cpu_load(cpu_pct);

    client_info_t clients[CLIENT_TRACK_MAX_CLIENTS];
    int count = 0;
    client_track_get_snapshot(clients, CLIENT_TRACK_MAX_CLIENTS, &count);
    uint32_t rx_total = 0, tx_total = 0;
    for (int i = 0; i < count; i++) {
        rx_total += clients[i].rx_bps;
        tx_total += clients[i].tx_bps;
    }
    uint32_t rx_pps = 0, tx_pps = 0;
    client_track_get_traffic_pps(&rx_pps, &tx_pps);

    const esp_app_desc_t *app_desc = esp_app_get_description();

    char resp[400];
    int len = snprintf(resp, sizeof(resp),
                        "{\"uptime_s\":%llu,\"free_heap\":%u,\"min_free_heap\":%u,"
                        "\"cpu_pct\":[%u,%u],\"cpu_freq_mhz\":%u,\"net_rx_bps\":%u,\"net_tx_bps\":%u,"
                        "\"net_rx_pps\":%u,\"net_tx_pps\":%u,"
                        "\"traffic_drops\":%u,\"version\":\"%s\"}",
                        (unsigned long long)(esp_timer_get_time() / 1000000ULL),
                        (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                        (unsigned)cpu_pct[0], (unsigned)cpu_pct[1], (unsigned)sys_monitor_get_cpu_freq_mhz(),
                        (unsigned)rx_total, (unsigned)tx_total, (unsigned)rx_pps, (unsigned)tx_pps,
                        (unsigned)client_track_get_traffic_drops(), app_desc->version);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

httpd_handle_t web_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    // Default 4KB is too tight once the OTA handler's call chain into
    // esp_ota_write()/SPI flash is included on this same worker task's stack.
    config.stack_size = 8192;
    // Default (8) is one short of the 9 routes registered below - the 9th
    // registration (ota_uri, being last) would silently fail to register,
    // leaving /api/ota unhandled with no error logged.
    config.max_uri_handlers = 12;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return NULL;
    }

    const httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_get_handler};
    const httpd_uri_t admin_page_uri = {.uri = "/admin", .method = HTTP_GET, .handler = admin_get_handler};
    const httpd_uri_t status_uri = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler};
    const httpd_uri_t wifi_uri = {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post_handler};
    const httpd_uri_t admin_uri = {.uri = "/api/admin", .method = HTTP_POST, .handler = admin_post_handler};
    const httpd_uri_t clients_uri = {.uri = "/api/clients", .method = HTTP_GET, .handler = clients_get_handler};
    const httpd_uri_t history_uri = {.uri = "/api/client/history", .method = HTTP_GET, .handler = client_history_get_handler};
    const httpd_uri_t system_uri = {.uri = "/api/system", .method = HTTP_GET, .handler = system_get_handler};
    const httpd_uri_t ota_uri = {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post_handler};

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &admin_page_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &wifi_uri);
    httpd_register_uri_handler(server, &admin_uri);
    httpd_register_uri_handler(server, &clients_uri);
    httpd_register_uri_handler(server, &history_uri);
    httpd_register_uri_handler(server, &system_uri);
    httpd_register_uri_handler(server, &ota_uri);

    ESP_LOGI(TAG, "Web server started");
    return server;
}
