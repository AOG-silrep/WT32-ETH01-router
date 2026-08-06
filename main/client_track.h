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

// How many duplicate-address conflicts can be tracked at once. Four is not a
// capacity estimate - one is already an unusual fault, and a network with five
// simultaneous address collisions has a problem that this device reporting the
// fifth would not help with. It is small on purpose: the set is scanned on the
// packet forwarding path.
#define CLIENT_TRACK_MAX_CONFLICTS 4

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
// queue was full (i.e. the accounting task couldn't keep up).
//
// This counts *accounting* events, never forwarded frames: a full queue costs
// the traffic nothing, both wrappers hand the pbuf on regardless. A large
// number means the per-client byte and packet figures undercount, and nothing
// worse. (The one thing that does stop a frame is the duplicate-address
// quarantine further down, which is counted separately by
// client_track_get_quarantine_drops().)
//
// It is normal for it to be large under load, and it tracks packet rate rather
// than any kind of fault. Measured on this device, with no data lost in any of
// them: nothing at 2 Mbit/s, ~4k over eight seconds at 20 Mbit/s, ~33k at 40,
// and 50-130k across a three-run throughput test. An idle bridge sits at 0.
// So it is not a health indicator and a climbing count is not a fault; it says
// the accounting task lost the race with the traffic, which is what it is
// designed to do rather than block forwarding.
//
// The counter is also approximate by construction - see the note in
// client_track.c on why it is not atomic.
uint32_t client_track_get_traffic_drops(void);

// Bridge-wide packet rate (all tracked clients summed, every protocol -
// TCP/UDP/ARP/etc. alike), updated once per second alongside rx_bps/tx_bps.
// Thread-safe; intended to be called from the HTTP server task.
void client_track_get_traffic_pps(uint32_t *rx_pps, uint32_t *tx_pps);

// Frames the bridge itself refused to carry, per port and direction. Unlike
// client_track_get_traffic_drops() above - which counts accounting events this
// file gave up on, and costs the traffic nothing - every one of these is a
// frame that was *not forwarded*, so a climbing count here is a real fault.
//
// *_in  is the tcpip mailbox rejecting an inbound frame from that port;
// *_out  is that port's driver having no buffer to transmit into.
//
// Neither drop is counted anywhere in lwIP or ESP-IDF; see the long note in
// client_track.c for where each one happens and why nothing else sees it.
// Approximate by construction, like the other counter and for the same reason.
typedef struct {
    uint32_t eth_in, wifi_in;
    uint32_t eth_out, wifi_out;
} client_track_fwd_drops_t;

void client_track_get_forward_drops(client_track_fwd_drops_t *out);

// Registers the WiFi tx-done callback that feeds client_track_get_wifi_tx().
// Must be called *after* esp_wifi_start(), unlike client_track_init() which
// must run before it - the underlying API refuses with
// ESP_ERR_WIFI_NOT_STARTED.
esp_err_t client_track_wifi_txdone_init(void);

// Frames the radio was asked to send, and how many of them it gave up on after
// exhausting 802.11 retries. This is the only visibility into on-air loss:
// the *_out counters above see a frame refused a buffer, but a frame accepted
// into the WiFi queue and then lost on air is discarded inside the driver
// without any layer above hearing about it.
//
// Read as a ratio, never as a bare failure count.
void client_track_get_wifi_tx(uint32_t *total, uint32_t *failed);

// ---- duplicate-address quarantine ----
//
// Two devices on one address - almost always a static address configured by
// hand inside the DHCP pool - leaves both of them half-working, because every
// other host's ARP cache keeps flipping between the two MACs. Neither device
// reports anything, and the symptom looks like bad wiring.
//
// So the bridge picks one and stops carrying the other's traffic: whichever MAC
// was on the address first keeps it, and the later arrival is cut off until it
// moves. One dead device is a fault somebody can find; two intermittent ones
// are not. DHCP is carried in both directions even for a cut-off device, so a
// device reconfigured to DHCP can always get itself out.
//
// The decision is derived state, rebuilt from the lease table once a second and
// never written to flash.

typedef struct {
    uint8_t mac[6];        // the device being cut off
    uint8_t peer_mac[6];   // the device that keeps the address
    esp_ip4_addr_t ip;     // the contested address
    bool armed;            // past the confirmation delay, i.e. a real conflict
    bool dropping;         // armed *and* enforcement is on
    bool exempt;           // pardoned from the console
    uint32_t armed_s;      // seconds since it armed, 0 if it hasn't
} client_track_conflict_t;

// Copies up to max current conflicts into out, including ones still inside the
// confirmation delay (armed == false). Returns the number written. Thread-safe.
int client_track_get_conflicts(client_track_conflict_t *out, int max);

// How many conflicts are currently armed - the number worth showing as a fault.
// Cheaper than taking the whole snapshot when only the count is wanted.
int client_track_get_armed_conflicts(void);

// Frames not forwarded because their sender or recipient is cut off. Unlike
// client_track_get_forward_drops(), a climbing count here is not a fault in the
// bridge - it is the bridge doing what it was told. Approximate by
// construction, like the other counters and for the same reason.
void client_track_get_quarantine_drops(uint32_t *uplink, uint32_t *downlink);

// Pardons mac: lifts its quarantine now and stops it being re-armed. Pass NULL
// to withdraw every pardon. Not persisted - see the note in client_track.c.
// Returns false only if there is no room to record another pardon.
bool client_track_quarantine_clear(const uint8_t mac[6]);

// Turns the dropping on or off. Detection, logging and everything reported
// above carry on either way, so switching it off leaves the diagnosis intact
// and only stops the enforcement acting on it.
void client_track_quarantine_enforce(bool on);
bool client_track_quarantine_enforcing(void);

#ifdef __cplusplus
}
#endif
