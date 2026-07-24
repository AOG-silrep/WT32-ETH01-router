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

// Fine-grained traffic history, sampled independently of the 1Hz rx_bps/tx_bps
// figures below, so a client's page-level graph can show bursts shorter than
// one second without waiting on (or disturbing) the slower IP/hostname tick.
#define CLIENT_TRACK_HISTORY_PERIOD_MS 100
#define CLIENT_TRACK_HISTORY_LEN 32   // 32 * 100ms = 3.2s buffered per client

typedef struct {
    uint8_t mac[6];
    esp_ip4_addr_t ip;   // 0.0.0.0 if not yet resolved
    bool is_wifi;        // true = WiFi station, false = wired/Ethernet
    int8_t rssi;         // valid only when is_wifi
    uint32_t rx_bps;     // bridge -> client, bytes/sec (downlink)
    uint32_t tx_bps;     // client -> bridge, bytes/sec (uplink)
    uint32_t rx_pps;     // bridge -> client, packets/sec (downlink), any protocol
    uint32_t tx_pps;     // client -> bridge, packets/sec (uplink), any protocol
    uint32_t last_seen_s; // seconds since this client's last observed traffic
    char name[CLIENT_TRACK_NAME_MAX_LEN]; // DHCP hostname (option 12); empty if unknown
} client_info_t;

typedef struct {
    uint32_t seq;      // sequence number of the newest sample included
    bool reset;        // true if the requested `since_seq` predates what's buffered
    int count;         // number of samples actually returned, oldest..newest
    uint32_t rx_bps[CLIENT_TRACK_HISTORY_LEN];
    uint32_t tx_bps[CLIENT_TRACK_HISTORY_LEN];
    uint32_t rx_pps[CLIENT_TRACK_HISTORY_LEN];
    uint32_t tx_pps[CLIENT_TRACK_HISTORY_LEN];
} client_history_t;

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

// Fills out with the fine-grained history samples for mac newer than
// since_seq (pass 0 to get everything currently buffered). Returns false if
// mac isn't a currently-tracked (active) client. Thread-safe; intended to be
// called from the HTTP server task.
bool client_track_get_history(const uint8_t mac[6], uint32_t since_seq, client_history_t *out);

// Count of traffic events dropped because the hot-path -> accounting-task
// queue was full (i.e. the accounting task couldn't keep up). Should stay at
// or near 0 under normal load; a climbing count means
// TRAFFIC_EVENT_QUEUE_DEPTH in client_track.c needs to be raised.
uint32_t client_track_get_traffic_drops(void);

// Bridge-wide packet rate (all tracked clients summed, every protocol -
// TCP/UDP/ARP/etc. alike), updated once per second alongside rx_bps/tx_bps.
// Thread-safe; intended to be called from the HTTP server task.
void client_track_get_traffic_pps(uint32_t *rx_pps, uint32_t *tx_pps);

#ifdef __cplusplus
}
#endif
