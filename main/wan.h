#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

// WiFi as WAN: a routed, NAT'd second interface, plus the
// packet filter that decides what may cross it.
//
// This is deliberately NOT an extension of the bridge. A WiFi station cannot be
// an L2 bridge port - 802.11 station frames carry three addresses, so a station
// cannot forward on behalf of other MACs, and esp_netif_br_glue only accepts an
// AP netif anyway. So the WAN is a separate subnet reached by routing, with
// NAPT on the bridge netif, and the LAN stays 192.168.5.0/24 exactly as before.
//
// What that costs, and why the UI says so out loud: the ESP32 has one 2.4 GHz
// radio. In WIFI_MODE_APSTA the SoftAP is forced onto the upstream AP's channel,
// so every associated client is dropped and re-associates, and the configured
// channel (1/6/11) becomes advisory while the WAN is up.
//
// What it does not cost: the L2 bridged path. bridgeif_input() forwards a
// unicast frame whose destination is not a local bridge MAC straight to
// bridgeif_send_to_ports() and frees it - it only reaches ip4_input() for group
// addresses bound for the CPU, or a local MAC. LAN-to-LAN traffic therefore
// never enters the IP layer, and enabling CONFIG_LWIP_IP_FORWARD adds nothing
// to it.

typedef enum {
    WAN_STATE_DISABLED = 0,     // no uplink configured; the device behaves as it always did
    WAN_STATE_CONNECTING,
    WAN_STATE_UP,
    WAN_STATE_DOWN,             // was up, lost the link, retrying
    WAN_STATE_NO_AP,            // scanned, upstream network not found
    WAN_STATE_AUTH_FAILED,      // wrong password, or the handshake kept timing out
    WAN_STATE_SUBNET_CONFLICT,  // upstream handed us an address overlapping the LAN
} wan_state_t;

typedef struct {
    wan_state_t    state;
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
    esp_ip4_addr_t dns;
    uint8_t        ap_channel;             // the channel the radio is ACTUALLY on
    uint8_t        ap_channel_configured;  // what wifi_cfg asked for
    int8_t         rssi;
    uint32_t       connects;
    uint32_t       disconnects;
    uint16_t       last_reason;            // 802.11 reason code of the last disconnect
    uint32_t       retry_in_s;             // 0 when no retry is pending
    uint32_t       tx_allowed;
    uint32_t       tx_blocked;
    uint32_t       rx_allowed;
    uint32_t       rx_blocked;
    bool           napt_on;
} wan_status_t;

// Brings up the WAN if one is configured, and does nothing at all if not.
//
// Must be called after esp_wifi_init() and setup_bridge(), and BEFORE
// esp_wifi_start(): it registers the WIFI_EVENT_STA_START handler that installs
// the filter hooks, and a handler registered after the event has fired would
// never see it. `br_netif` is the NAPT inside interface.
esp_err_t wan_init(esp_netif_t *br_netif);

// A snapshot of the current state, for the web API and the console.
void wan_get_status(wan_status_t *out);

// The upstream resolver to hand LAN clients in DHCP option 6, in network byte
// order, or 0 when the WAN is not up. Reads a published snapshot: it takes no
// lock and cannot block, so the DHCP server task can call it while building a
// reply.
uint32_t wan_get_dns(void);

bool wan_is_up(void);

// True when an uplink is configured at all, whatever state it has reached -
// connecting, retrying, or failed authentication all count. Distinct from
// wan_is_up(), and the distinction is the point: "no uplink configured" and
// "an uplink that has not got there yet" look identical to anything that only
// asks whether the WAN is up, and they call for opposite behaviour in
// dhcp_server.c's lease length.
//
// Lock-free, like wan_is_up() and wan_get_dns() above, so the DHCP server task
// can call it while building a reply.
bool wan_is_enabled(void);

// Re-reads the allowed-port list from wan_cfg and republishes it to the filter.
// Takes effect on the next packet; no reboot, unlike an SSID change.
void wan_ports_changed(void);

const char *wan_state_name(wan_state_t s);

#ifdef __cplusplus
}
#endif
