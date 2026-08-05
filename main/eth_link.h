#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// State of the Ethernet port itself - what the PHY negotiated with whatever
// is on the other end of the cable, as opposed to which bridge port a client
// happens to sit on (that's the "link" field in /api/clients).
//
// The case this exists for: a LAN8720 that came up at 10 Mbit half-duplex
// over a marginal or miswired cable. Nothing errors, the bridge just gets
// slow, and from a WiFi client that looks exactly like a bad radio link.
typedef struct {
    bool     up;              // link currently up
    uint32_t speed_mbit;      // 10 or 100; 0 while the link is down
    bool     full_duplex;     // only meaningful while up
    bool     autoneg;         // autonegotiation configured on the PHY
    uint32_t flaps;           // link-down transitions since boot
    uint32_t since_change_s;  // seconds held in the current up/down state
} eth_link_status_t;

// Starts tracking Ethernet link state by registering for the driver's
// connect/disconnect events. Must be called before esp_eth_start(), or the
// first link-up is missed and the port reads as down until the cable is
// next unplugged.
//
// Takes no handle: ETHERNET_EVENT_CONNECTED carries the esp_eth_handle_t as
// its event data, which is the only thing this module needs it for.
esp_err_t eth_link_init(void);

// Copies the current port state into out. Speed and duplex are whatever the
// PHY reported at the last link-up; they are not read from hardware here.
// Thread-safe; intended to be called from the HTTP server and console tasks.
void eth_link_get_status(eth_link_status_t *out);

#ifdef __cplusplus
}
#endif
