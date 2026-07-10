#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLIENT_TRACK_MAX_CLIENTS 16
#define CLIENT_TRACK_NAME_MAX_LEN 32

typedef struct {
    uint8_t mac[6];
    esp_ip4_addr_t ip;   // 0.0.0.0 if not yet resolved
    bool is_wifi;        // true = WiFi station, false = wired/Ethernet
    int8_t rssi;         // valid only when is_wifi
    uint32_t rx_bps;     // bridge -> client, bytes/sec (downlink)
    uint32_t tx_bps;     // client -> bridge, bytes/sec (uplink)
    uint32_t last_seen_s; // seconds since this client's last observed traffic
    char name[CLIENT_TRACK_NAME_MAX_LEN]; // DHCP hostname (option 12); empty if unknown
} client_info_t;

// Hooks the eth/wifi bridge ports for per-client byte counting and starts
// the periodic accounting/aging/IP-resolution task. Must be called after
// setup_bridge() (so br_netif exists) but before esp_eth_start()/
// esp_wifi_start() - it registers ETHERNET_EVENT_START/WIFI_EVENT_AP_START
// handlers that run after the bridge glue's own handlers to capture the
// bridge's already-installed netif->input before wrapping it.
esp_err_t client_track_init(esp_netif_t *eth_netif, esp_netif_t *wifi_netif,
                             esp_netif_t *br_netif, const uint8_t *bridge_mac);

// Copies up to max active client entries into out (in no particular order).
// Thread-safe; intended to be called from the HTTP server task.
void client_track_get_snapshot(client_info_t *out, int max, int *count);

#ifdef __cplusplus
}
#endif
