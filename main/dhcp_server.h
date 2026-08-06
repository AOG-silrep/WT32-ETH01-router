#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

// How many MAC -> IP reservations are remembered across a reboot. Above
// CLIENT_TRACK_MAX_CLIENTS (16), which is what this device claims it can show,
// and well inside the 100-address pool, so the table is not the first thing to
// give out. Each stored entry costs 10 bytes of NVS, so the whole table is a
// few hundred bytes of a 24KB partition.
#define DHCP_SERVER_MAX_LEASES 32

typedef struct {
    uint8_t mac[6];
    esp_ip4_addr_t ip;           // address this server leased; 0 if it never leased one
    esp_ip4_addr_t observed_ip;  // address the client actually uses, if not the leased one
    uint32_t expires_in_s;  // 0 if the lease has expired or was only restored from flash
    // Boot generations since this client was last heard from: 0 means it has
    // been seen since the device booted, higher means it is that many
    // table-writing boots stale. This is both the reclaim order - oldest first,
    // when something else needs the room - and the forgetting rule: an entry
    // that reaches FORGET_AFTER_BOOTS is dropped at the next load. Never a
    // clock; see the note on s_boot_seq in dhcp_server.c for why elapsed time
    // is not measurable here.
    uint32_t boots_since_seen;
    bool stored;            // true once this mapping is committed to NVS
    // What flash currently holds for this MAC, as of the last successful
    // commit - not what the live fields above say. While a change is waiting
    // out the save debounce the two differ, and that difference is the whole
    // point: a client that just took a new address still has the old one
    // stored, and would come back to it if the device lost power now.
    esp_ip4_addr_t saved_ip;           // ip as last committed; 0 if never
    esp_ip4_addr_t saved_observed_ip;  // observed_ip as last committed
} dhcp_lease_info_t;

// Starts the DHCP server for the bridge LAN: restores the saved MAC -> IP
// reservations from NVS, then binds UDP port 67 and serves from a task of its
// own.
//
// This replaces ESP-IDF's DHCP server rather than supplementing it. The bridge
// netif must have been created *without* ESP_NETIF_DHCP_SERVER in its flags -
// see setup_bridge() in main.c - or IDF's server will already hold port 67.
//
// The bridge netif does not have to be up, or even started: the socket binds
// to INADDR_ANY, and the server pins itself to the bridge later, when the
// bridge reports itself up. What this does need is br_netif already created,
// esp_event_loop_create_default() already called (it registers handlers), and
// to run before client_track_init(), which depends on this module for address
// lookups.
//
// [pool_start, pool_end] serves clients that ask over WiFi, and
// [wired_start, wired_end] those that ask over the Ethernet port - which is
// established per request by client_track.c, since the bridge has merged the two
// ports long before a DHCP message gets here. One server, not two: the bridge is
// a single broadcast domain with one netif and one UDP/67 socket, so a second
// server could not be handed its own traffic even in principle.
//
// The wired range must be on the bridge's subnet and must not overlap
// [pool_start, pool_end] or contain ip. Pass 0/0 to serve one range to
// everybody; a range that fails those rules is logged and treated as 0/0 rather
// than refused, since the caller cannot usefully recover from a wrong
// compile-time constant at boot.
//
// A client that changes ports is renumbered into the range for the port it is
// now on. Both ranges are remembered across a reboot in the ordinary way.
esp_err_t dhcp_server_start(esp_netif_t *br_netif,
                            esp_ip4_addr_t ip, esp_ip4_addr_t netmask,
                            esp_ip4_addr_t pool_start, esp_ip4_addr_t pool_end,
                            esp_ip4_addr_t wired_start, esp_ip4_addr_t wired_end);

// Records that mac was seen using ip, whether or not this server leased it.
// A client that assigns itself an address never tells the DHCP server about
// it, so without this the remembered mapping would be the lease it ignored
// rather than the address it actually uses.
//
// Addresses off the bridge's subnet are discarded: this is a transparent
// bridge, so frames from other subnets cross it legitimately, and a client
// that self-assigns a 169.254 link-local after failing DHCP is not reporting
// an address worth keeping either.
//
// Cheap and idempotent - it writes to flash only when the mapping actually
// changes, so calling it on every observation is fine. Thread-safe; called
// from client_track.c, which is the one place a client's real address is
// established.
void dhcp_server_note_observed_ip(const uint8_t mac[6], esp_ip4_addr_t ip);

// Replaces the set of MACs currently cut off for holding a duplicate address.
// The whole set every time, not a delta - anything absent from macs is cleared.
//
// Two things follow from an entry being in this set. Its claim on the address
// stops counting in the allocator, so the device that was there first can still
// be leased it (without this the decision would reverse at every reboot, since
// leases come back from flash expired while observed addresses come back
// intact). And the address is no longer confirmed back to the quarantined
// client, so a client that speaks DHCP gets NAKed onto a free address instead
// and the conflict resolves itself.
//
// Not persisted. Called from client_track.c, which is where the conflict is
// detected - see the note there on lock order. Thread-safe.
void dhcp_server_set_quarantined(const uint8_t (*macs)[6], int n);

// Looks up the address this MAC is believed to be at: what it was last
// observed using if that differs from its lease, otherwise the leased
// address. Returns false if this MAC has no lease and nothing observed, which
// includes a reservation restored from flash that the client hasn't claimed
// yet - an address nobody has taken up is not one to report as theirs.
//
// This is what client_track.c calls in place of
// esp_netif_dhcps_get_clients_by_mac(), which only answers for IDF's own
// server. Thread-safe.
bool dhcp_server_lookup_ip(const uint8_t mac[6], esp_ip4_addr_t *out_ip);

// Copies up to max lease entries into out, including reservations restored
// from flash that no client has claimed yet (those report expires_in_s 0).
// Returns the number written. Thread-safe; intended for the serial console and
// the web UI's leases page.
int dhcp_server_get_leases(dhcp_lease_info_t *out, int max);

// Number of reservations read back from NVS at startup, for the boot log.
int dhcp_server_get_restored_count(void);

#ifdef __cplusplus
}
#endif
