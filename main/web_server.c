#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "web_server.h"
#include "wifi_cfg.h"
#include "client_track.h"
#include "sys_monitor.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web_server";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

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

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                     index_html_end - index_html_start - 1);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
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
    json_get_string(buf, "password", password, sizeof(password)); // absent -> open network

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

static esp_err_t clients_get_handler(httpd_req_t *req)
{
    client_info_t clients[CLIENT_TRACK_MAX_CLIENTS];
    int count = 0;
    client_track_get_snapshot(clients, CLIENT_TRACK_MAX_CLIENTS, &count);

    static char resp[CLIENT_TRACK_MAX_CLIENTS * (160 + CLIENT_TRACK_NAME_MAX_LEN) + 16];
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
                         "\"last_seen_s\":%u,\"name\":\"%s\"}",
                         i == 0 ? "" : ",",
                         c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5],
                         ip_str, c->is_wifi ? "wifi" : "eth", rssi_str,
                         (unsigned)c->rx_bps, (unsigned)c->tx_bps, (unsigned)c->last_seen_s,
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

    static char resp[900];
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
    off += snprintf(resp + off, sizeof(resp) - off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, off);
    return ESP_OK;
}

static esp_err_t system_get_handler(httpd_req_t *req)
{
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

    char resp[256];
    int len = snprintf(resp, sizeof(resp),
                        "{\"uptime_s\":%llu,\"free_heap\":%u,\"min_free_heap\":%u,"
                        "\"cpu_pct\":[%u,%u],\"net_rx_bps\":%u,\"net_tx_bps\":%u,"
                        "\"client_count\":%d}",
                        (unsigned long long)(esp_timer_get_time() / 1000000ULL),
                        (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                        (unsigned)cpu_pct[0], (unsigned)cpu_pct[1],
                        (unsigned)rx_total, (unsigned)tx_total, count);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

httpd_handle_t web_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return NULL;
    }

    const httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_get_handler};
    const httpd_uri_t status_uri = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler};
    const httpd_uri_t wifi_uri = {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post_handler};
    const httpd_uri_t clients_uri = {.uri = "/api/clients", .method = HTTP_GET, .handler = clients_get_handler};
    const httpd_uri_t history_uri = {.uri = "/api/client/history", .method = HTTP_GET, .handler = client_history_get_handler};
    const httpd_uri_t system_uri = {.uri = "/api/system", .method = HTTP_GET, .handler = system_get_handler};

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &wifi_uri);
    httpd_register_uri_handler(server, &clients_uri);
    httpd_register_uri_handler(server, &history_uri);
    httpd_register_uri_handler(server, &system_uri);

    ESP_LOGI(TAG, "Web server started");
    return server;
}
