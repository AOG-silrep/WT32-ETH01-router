#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

// Ships the log ring to a collector as RFC 5424 datagrams over UDP.
//
// The ring in log_buf.c is 128 lines of RAM: a live tail, not a record. It
// starts empty at every boot and whatever explained the last one is gone. This
// module gives those lines somewhere to go that outlives the device.
//
// The property that matters most, and the one the design is arranged around:
// while the bridge network is down the cursor does not move, so the entire boot
// sequence is still queued when the link comes up and ships in order. A sender
// that advanced its cursor regardless would be one that never manages to
// deliver the only lines nobody can otherwise see.
//
// What this cannot do, and no amount of code here would fix: confirm delivery.
// UDP has no acknowledgement, and this device has no route to anywhere that
// could tell it. `sent` below means "handed to the network" and nothing more.
//
// TIMESTAMP is the RFC 5424 NILVALUE, so the collector stamps on receipt. There
// is usually no clock here to do better with - no RTC battery, and esp_timer
// restarts at zero every boot. A WAN can now supply one (clock_time.h), but
// deliberately not to this: a log ring whose lines are relative for the first
// forty seconds of a boot and absolute afterwards is harder to read than one
// that is consistently relative, and the collector's receipt stamp is already
// better than either for lines that reach it. What the clock did get used for
// is the reset history, where each record is a single dated event rather than a
// stream - see reset_log.h.

typedef struct {
    bool     enabled;     // configuration says on
    bool     bound;       // socket exists and is pinned to the bridge right now
    uint32_t sent;        // datagrams lwIP accepted
    uint32_t filtered;    // lines below the configured severity, deliberately not sent
    uint32_t aged_out;    // lines that left the ring before they could be sent
    uint32_t send_fail;   // sendto() calls that returned an error
    int      last_errno;  // errno from the most recent failure, 0 if none yet
    uint32_t backlog;     // lines waiting to be sent right now
} syslog_status_t;

// Starts the sender. `br_netif` is the bridge, used to pin the socket to it and
// to read the subnet for syslog_get_subnet().
//
// Must be called before esp_eth_start()/esp_wifi_start(), like dhcp_server_start()
// and eth_link_init(): it registers for IP_EVENT_NETIF_UP, and a registration
// made after the bridge came up would never see the one event it needs.
//
// Returns an error only for a resource failure. A configuration that is absent,
// disabled or unusable is ESP_OK - the sender simply has nothing to do.
esp_err_t syslog_init(esp_netif_t *br_netif);

// Current state and counters, for /api/logs, /api/syslog and the console.
void syslog_get_status(syslog_status_t *out);

// Tells the sender its configuration was saved, so the change lands in
// milliseconds instead of at the next idle tick. Purely a latency
// optimisation - the sender compares syslog_cfg_generation() on every wake, so
// forgetting this call costs a second, not correctness.
void syslog_config_changed(void);

// The bridge's own address and netmask, for syslog_cfg_validate()'s on-subnet
// check. Read off the netif rather than from main.c's BRIDGE_IP/BRIDGE_NETMASK,
// so the check follows the address the device actually has rather than the one
// it was compiled with. Both are set to 0 if the netif cannot be read, which
// syslog_cfg_validate() treats as "cannot check" and refuses on.
void syslog_get_subnet(uint32_t *ip, uint32_t *netmask);

#ifdef __cplusplus
}
#endif
