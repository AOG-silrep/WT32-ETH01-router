#include <string.h>
#include "client_track.h"
#include "dhcp_server.h"
#include "esp_netif_net_stack.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"   // esp_wifi_set_tx_done_cb()
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip.h"    // IP_PROTO_UDP
#include "lwip/prot/ip4.h"
#include "netif/ethernet.h"

static const char *TAG = "client_track";

#define CLIENT_AGE_OUT_US (5LL * 60 * 1000000)   // drop a client after 5 min of silence
#define TICK_PERIOD_MS    1000
#define FAST_TICKS_PER_SLOW_TICK (TICK_PERIOD_MS / CLIENT_TRACK_HISTORY_PERIOD_MS)
#define TRAFFIC_EVENT_QUEUE_DEPTH 64

// Everything account_traffic() needs, captured by value - the hot-path
// wrappers below run in the eth/wifi driver's own RX/TX task, and the pbuf
// (and anything pointing into it) is not valid once that task moves on, so
// nothing here may be a pointer into pbuf data.
typedef struct {
    uint8_t mac[6];
    bool is_wifi;
    uint16_t bytes;
    bool is_uplink;
    esp_ip4_addr_t observed_ip; // .addr == 0 means "none observed"
} traffic_event_t;

typedef struct {
    bool active;
    uint8_t mac[6];
    esp_ip4_addr_t ip;
    bool is_wifi;
    int8_t rssi;
    uint32_t rx_bytes, tx_bytes;           // cumulative
    uint32_t rx_bytes_prev, tx_bytes_prev; // previous 1Hz tick's cumulative, for rate calc
    uint32_t rx_bps, tx_bps;
    uint32_t rx_pkts, tx_pkts;             // cumulative, one per accounted frame regardless of protocol
    uint32_t rx_pkts_prev, tx_pkts_prev;   // previous 1Hz tick's cumulative, for rate calc
    uint32_t rx_pps, tx_pps;
    int64_t last_seen_us;
    // When this client's own traffic first confirmed the address it currently
    // holds, or 0 if it never has. The ordering key for a duplicate-address
    // conflict: whoever got to the address first keeps it.
    //
    // 0 means "never seen there", not "seen there long ago". An address the
    // bridge only believes - restored from flash, read out of the ARP cache, or
    // the one the DHCP server offered - carries no time, because none of those
    // say the client is on it now, let alone when it got there. A device is
    // never cut off on that basis; quarantine_rebuild() requires a real time
    // from both sides before it will act.
    int64_t ip_since_us;
    char name[CLIENT_TRACK_NAME_MAX_LEN];

    // Fine-grained history, sampled every CLIENT_TRACK_HISTORY_PERIOD_MS - kept
    // fully independent of rx_bytes_prev/tx_bytes_prev above so the two
    // cadences never share bookkeeping, both just diffing the same raw
    // cumulative rx_bytes/tx_bytes.
    uint32_t rx_bytes_prev_fast, tx_bytes_prev_fast;
    uint32_t rx_pkts_prev_fast, tx_pkts_prev_fast;
    uint32_t hist_rx[CLIENT_TRACK_HISTORY_LEN]; // bytes/sec, one per CLIENT_TRACK_HISTORY_PERIOD_MS bucket
    uint32_t hist_tx[CLIENT_TRACK_HISTORY_LEN];
    uint32_t hist_rx_pps[CLIENT_TRACK_HISTORY_LEN]; // packets/sec, same buckets as hist_rx/hist_tx
    uint32_t hist_tx_pps[CLIENT_TRACK_HISTORY_LEN];
    uint32_t hist_seq; // count of fast samples ever produced for this client
} client_entry_t;

static client_entry_t s_clients[CLIENT_TRACK_MAX_CLIENTS];
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_traffic_q;
// Deliberately a plain increment, and deliberately racy. The two hot-path
// wrappers below reach this from different tasks on different cores, so a ++ is
// a read-modify-write that loses counts when two land together - exactly when
// the queue is overflowing and the number is being looked at.
//
// Making it _Atomic was tried and backed out on measurement. ESP-IDF has no
// lock-free 32-bit RMW for dual-core Xtensa: __atomic_fetch_add_4 goes through
// _ATOMIC_ENTER_CRITICAL, which is portENTER_CRITICAL_SAFE on one global
// spinlock shared by every atomic in the system (esp_libc's esp_stdatomic.h).
// Taking that inside the RX/TX path, ~50k times per test run, cost 35% of
// uplink throughput: 24.8 Mbit/s fell to 16.1, reproduced across two runs
// either way (bench/04-atomic-drops vs bench/04b-plain-counter-control).
//
// That is a bad trade for this counter. It is a diagnostic that is only ever
// displayed, and it is read to answer "is the accounting queue overflowing",
// where a few lost counts out of tens of thousands changes nothing. Throughput
// is what this device is for. If an exact count is ever genuinely needed, the
// way to get it is per-core counters summed on read, not a shared atomic.
static volatile uint32_t s_traffic_drops;

// Frames the *bridge* refused, as opposed to accounting events this file lost.
// Indexed [0] = eth port, [1] = wifi port, matching the is_wifi flag the
// wrappers already carry. Racy for the same reason s_traffic_drops is, and
// acceptable for the same reason: these are diagnostics, and the hot path they
// sit in is the one thing this device must not slow down.
//
// There are two silent drop points on the forwarding path and neither is
// counted anywhere in lwIP or ESP-IDF:
//
//   in[]   lwIP's bridgeif does not forward in the caller's context. With
//          BRIDGEIF_PORT_NETIFS_OUTPUT_DIRECT == 0 (which is what ESP-IDF
//          builds, since it defaults to NO_SYS) bridgeif_add_port() points
//          each port's input at bridgeif_tcpip_input(), which is
//          tcpip_inpkt() -> sys_mbox_trypost() onto the single tcpip mailbox.
//          trypost does not block: when the mailbox is full the frame is
//          dropped and ERR_MEM comes back. That mailbox is
//          CONFIG_LWIP_TCPIP_RECVMBOX_SIZE deep and is shared by *both*
//          directions plus the bridge's own IP stack, so a burst one way can
//          evict the other way's traffic.
//
//   out[]  esp_netif's low_level_output() returns ERR_MEM when the WiFi driver
//          has no TX buffer left, but bridgeif_input() throws away what
//          bridgeif_send_to_ports() returns and frees the pbuf regardless.
//
// Both wrappers below already call through to the function that reports these
// and already return its value; they just never looked at it.
static volatile uint32_t s_fwd_in_drops[2];
static volatile uint32_t s_fwd_out_drops[2];

// What the radio actually managed to deliver, which is the one thing the
// counters above cannot see. esp_wifi_internal_tx() returning ESP_OK only means
// the frame was accepted into the WiFi driver's queue; a frame that then
// exhausts its 802.11 retry limit is discarded inside the driver and no layer
// above is told. So s_fwd_out_drops[] counts buffers, not airtime, and a
// downlink could be losing every other frame on air with all of it reading zero.
//
// The tx-done callback is the only report of that. Both counters are kept, not
// just the failures, because a failure count on its own says nothing - six
// failures out of ten frames and six out of a million are different faults.
static volatile uint32_t s_wifi_tx_total;
static volatile uint32_t s_wifi_tx_failed;

// ---- duplicate-address quarantine, hot-path half ----
//
// When two MACs are found on one address (see quarantine_rebuild()), the one
// that got there second is cut off: the wrappers below stop forwarding its
// frames, so the collision becomes one working device and one plainly dead one
// instead of two that half-work. This is the table they check.
//
// Frames dropped here are counted apart from s_fwd_*_drops on purpose. Those
// mean the bridge failed to carry something it meant to carry; these are the
// bridge doing exactly what it was told.
static volatile uint32_t s_q_drops[2];   // [0] uplink (by source MAC), [1] downlink (by dest)

#define QUARANTINE_MAX CLIENT_TRACK_MAX_CONFLICTS

// A MAC as two aligned words, so every read below is a single l32i that cannot
// tear. hi's bit 31 marks the slot live, which is why only 16 of its bits carry
// address.
typedef struct {
    uint32_t lo;   // mac[0..3]
    uint32_t hi;   // mac[4..5] in bits 0..15, bit 31 = slot in use
} q_slot_t;

#define Q_SLOT_LIVE 0x80000000u

// Read per frame on both cores, written once a second at most by
// client_track_task and by nobody else. A mutex is not an option - the input
// wrapper runs in the driver's own RX context, where blocking is not allowed -
// and neither is _Atomic, for the measured reason on s_traffic_drops above.
//
// So: a seqlock over the whole table, checked *after* the compare.
//
// Per-slot valid bits would not be enough. A reader can take lo from one
// version of the table and hi from the next and end up holding a MAC that
// belongs to neither entry - and with several identical modules from one vendor
// batch, whose first four octets match, the address it synthesises is very
// likely a real third device on this bridge rather than a number nobody owns.
// Validating one generation across the entire scan is what rules that out.
//
// A reader that sees the generation move gives up and forwards the frame. That
// asymmetry is the design: a missed drop costs one forwarded packet, once,
// while a wrong drop takes a working device off the network.
static volatile q_slot_t s_q[QUARANTINE_MAX];
static volatile uint32_t s_q_gen;    // odd while the table is being rewritten
// Whether anything is quarantined at all. The whole cost of this feature on an
// uncontended bridge is this one load and branch - cheaper than either memcmp
// mac_is_client() already does.
static volatile uint32_t s_q_any;

static uint32_t s_net_rx_pps, s_net_tx_pps; // bridge-wide, summed across clients each 1Hz tick

static esp_netif_t *s_eth_netif;
static esp_netif_t *s_wifi_netif;
static esp_netif_t *s_br_netif;
static uint8_t s_bridge_mac[6];

static netif_input_fn s_eth_orig_input;
static netif_linkoutput_fn s_eth_orig_linkoutput;
static netif_input_fn s_wifi_orig_input;
static netif_linkoutput_fn s_wifi_orig_linkoutput;

// Not broadcast/multicast (LSB of first octet), not all-zero, not the
// bridge's own MAC - i.e. a real, individual client address.
static bool mac_is_client(const uint8_t *mac)
{
    if (mac[0] & 0x01) {
        return false;
    }
    static const uint8_t zero[6] = {0};
    if (memcmp(mac, zero, 6) == 0) {
        return false;
    }
    if (memcmp(mac, s_bridge_mac, 6) == 0) {
        return false;
    }
    return true;
}

// True if this MAC is currently cut off for holding a duplicate address. Safe
// to call from either wrapper on either core; see s_q for the scheme.
//
// The MAC is assembled from six byte loads rather than by casting the pbuf
// pointer to uint32_t *. An ethernet header inside an esp_netif pbuf is only
// 2-byte aligned in the general case - the stack aligns the IP header, not this
// one - and an unaligned l32i on Xtensa is a LoadStoreAlignmentCause panic, not
// a slow path.
static inline bool mac_is_quarantined(const uint8_t *mac)
{
    if (s_q_any == 0) {
        return false;
    }
    uint32_t gen = s_q_gen;
    if (gen & 1u) {
        return false;   // mid-rewrite; forward it and catch it next frame
    }

    uint32_t lo = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
                  ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
    uint32_t hi = ((uint32_t)mac[4] | ((uint32_t)mac[5] << 8)) | Q_SLOT_LIVE;

    bool hit = false;
    for (int i = 0; i < QUARANTINE_MAX; i++) {
        if (s_q[i].lo == lo && s_q[i].hi == hi) {
            hit = true;
            break;
        }
    }
    // Only act on a match the whole table held still for. A hit read across a
    // rewrite may be an address that was never in the table at all.
    return hit && s_q_gen == gen;
}

// Publishes the armed set.
//
// Caller must hold s_mutex. That is what keeps this single-writer, which the
// scheme requires: two publishers interleaving their generation sequences would
// leave the counter even in the middle of a rewrite, and readers would trust a
// half-written table. The mutex is free here - this runs once a second at most,
// and never on the forwarding path.
static void quarantine_publish(const uint8_t (*macs)[6], int n)
{
    if (n > QUARANTINE_MAX) {
        n = QUARANTINE_MAX;
    }

    // Raised before the rewrite and lowered after it, so the window where the
    // two disagree always errs towards readers doing the full check.
    if (n > 0) {
        s_q_any = 1;
    }

    s_q_gen++;                              // -> odd: rewrite in progress
    __asm__ __volatile__("" ::: "memory");
    for (int i = 0; i < QUARANTINE_MAX; i++) {
        if (i < n) {
            s_q[i].lo = (uint32_t)macs[i][0] | ((uint32_t)macs[i][1] << 8) |
                        ((uint32_t)macs[i][2] << 16) | ((uint32_t)macs[i][3] << 24);
            s_q[i].hi = ((uint32_t)macs[i][4] | ((uint32_t)macs[i][5] << 8)) | Q_SLOT_LIVE;
        } else {
            s_q[i].lo = 0;
            s_q[i].hi = 0;
        }
    }
    __asm__ __volatile__("" ::: "memory");
    s_q_gen++;                              // -> even: readable again

    if (n == 0) {
        s_q_any = 0;
    }
}

// Caller must hold s_mutex. Finds an existing entry for mac, or claims a
// free slot and initializes it (including last_seen_us, so a client that's
// only just been observed - e.g. via the WiFi station list, before it has
// sent any traffic - isn't immediately aged out on the next tick).
static client_entry_t *find_or_create_locked(const uint8_t *mac)
{
    client_entry_t *free_slot = NULL;
    for (int i = 0; i < CLIENT_TRACK_MAX_CLIENTS; i++) {
        if (s_clients[i].active && memcmp(s_clients[i].mac, mac, 6) == 0) {
            return &s_clients[i];
        }
        if (!s_clients[i].active && free_slot == NULL) {
            free_slot = &s_clients[i];
        }
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        memcpy(free_slot->mac, mac, 6);
        free_slot->active = true;
        free_slot->last_seen_us = esp_timer_get_time();
    }
    return free_slot;
}

// Caller must hold s_mutex. Updates e->ip from a freshly-resolved address,
// logging a warning if this MAC now maps to a different IP than last
// observed. Ignores 0.0.0.0 (unresolved) so a transient DHCP/ARP lookup
// miss doesn't clobber a known-good IP or manufacture a spurious "change"
// on the next successful resolution.
//
// The first address learnt for a MAC is logged too. A station's join line says
// "no address yet" - which is normal, since DHCP only happens once it is
// associated - and without this nothing ever said otherwise, so the log never
// showed a client getting an address at all. how names where the address came
// from, because the four sources are not equally trustworthy: only the sniffed
// one is the client's own word for what it is using.
//
// Returning early when the address is unchanged is load-bearing, not just
// tidiness: account_traffic() reaches this once per accounted frame, and the
// formatting below must not run on that path.
static void update_client_ip_locked(client_entry_t *e, esp_ip4_addr_t new_ip, const char *how)
{
    if (new_ip.addr == 0 || new_ip.addr == e->ip.addr) {
        return;
    }
    char new_str[16];
    esp_ip4addr_ntoa(&new_ip, new_str, sizeof(new_str));
    if (e->ip.addr == 0) {
        ESP_LOGI(TAG, "client %02x:%02x:%02x:%02x:%02x:%02x has address %s (%s)",
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                 new_str, how);
    } else {
        char old_str[16];
        esp_ip4addr_ntoa(&e->ip, old_str, sizeof(old_str));
        ESP_LOGW(TAG, "client %02x:%02x:%02x:%02x:%02x:%02x changed IP: %s -> %s (%s)",
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                 old_str, new_str, how);
    }
    e->ip = new_ip;
    // A new address starts unconfirmed whatever it came from, including a
    // sniffed one - account_traffic() stamps it immediately afterwards, since
    // that is the one caller holding the client's own word for it. Doing it
    // here instead would miss the case that matters most: a client bootstrapped
    // to the right address never reaches this function again, because of the
    // early return above, so its traffic could confirm the address a thousand
    // times over and it would still look like it had never been seen on it.
    e->ip_since_us = 0;
}

// observed_ip is the client's real source IP as sniffed directly out of an
// uplink IPv4 packet (NULL if this frame wasn't IPv4, or for downlink
// traffic) - this is ground truth for what address the client is actually
// using, independent of what the DHCP server thinks it leased out.
static void account_traffic(const uint8_t *mac, bool is_wifi, uint16_t bytes, bool is_uplink,
                             const esp_ip4_addr_t *observed_ip)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    client_entry_t *e = find_or_create_locked(mac);
    if (e) {
        e->is_wifi = is_wifi;
        if (is_uplink) {
            e->tx_bytes += bytes;
            e->tx_pkts++;
        } else {
            e->rx_bytes += bytes;
            e->rx_pkts++;
        }
        e->last_seen_us = esp_timer_get_time();
        if (observed_ip != NULL) {
            // Compared before the update so the DHCP server is told only when
            // the address actually moved, not on every packet: this runs
            // per-frame, and a mutex plus a table scan each time would slow
            // the accounting task for nothing. In steady state the address
            // never changes and this costs one comparison.
            bool ip_is_new = (e->ip.addr != observed_ip->addr);
            update_client_ip_locked(e, *observed_ip, "seen in its own traffic");

            // The moment the client's own traffic first vouches for the address
            // held for it. This is the only place that can say it: every other
            // source is the bridge repeating what it believes - flash, the ARP
            // cache, or the address the server offered - and none of those is
            // evidence the client is really there.
            //
            // Deliberately not refreshed afterwards. It records when the client
            // was first seen on this address, not when it was last seen, and it
            // is what decides who keeps an address when two devices claim it:
            // whoever got there first. A value that moved with every packet
            // would make the answer alternate between them.
            if (e->ip_since_us == 0 && e->ip.addr == observed_ip->addr) {
                e->ip_since_us = esp_timer_get_time();
            }

            // Tell the DHCP server what this client is really using, so the
            // mapping it keeps in flash is the client's actual address rather
            // than a lease the client may have ignored in favour of one it set
            // itself. Deliberately only from here: this is the sniffed source
            // address, the one piece of ground truth. The other callers of
            // update_client_ip_locked() are bootstraps from the lease table
            // and from the DHCP-assigned event, and an assignment is the
            // server saying what it offered, not the client saying what it
            // took - feeding those back would let a client that renews a stale
            // lease in the background overwrite the static address it is on.
            //
            // Safe under s_mutex: dhcp_server.c takes only its own lock and
            // never calls back into this file, so the two are always acquired
            // in this order and cannot deadlock.
            if (ip_is_new) {
                dhcp_server_note_observed_ip(e->mac, *observed_ip);
            }
        }
    }
    xSemaphoreGive(s_mutex);
}

// True for a client's own DHCP traffic, which the source-IP sniffing below
// must skip.
//
// A DHCP packet's source address is what the client's DHCP task believes it
// holds, which is not the same as the address the client is using. A device
// given a static address by hand often leaves its DHCP client running, and
// those background renewals go out with the *leased* address as their source.
// Sniffing them flips the recorded address back and forth - lease, real
// address, lease - on every renewal and every re-association, and each flip
// rewrites the mapping in flash. Observed via a station rejoining:
//
//   client e8:9f:6d:55:5b:10 changed IP: 192.168.5.77 -> 192.168.5.3
//   client e8:9f:6d:55:5b:10 changed IP: 192.168.5.3 -> 192.168.5.77
//   dhcp_server: saved 2 MAC->IP reservations to flash
//
// avail is how many bytes are readable from iph onwards.
static bool is_dhcp_packet(const struct ip_hdr *iph, size_t avail)
{
    // IPH_HL != 5 means IP options, which put the UDP header somewhere other
    // than the fixed offset used below. DHCP clients do not send those, so
    // declining to classify such a packet is the right answer rather than a
    // gap.
    if (IPH_PROTO(iph) != IP_PROTO_UDP || IPH_HL(iph) != 5 || avail < IP_HLEN + 4) {
        return false;
    }
    const uint8_t *udp = (const uint8_t *)iph + IP_HLEN;
    uint16_t sport = (uint16_t)((udp[0] << 8) | udp[1]);
    uint16_t dport = (uint16_t)((udp[2] << 8) | udp[3]);
    return sport == 67 || sport == 68 || dport == 67 || dport == 68;
}

// Chain to the bridge's input and count what it refuses. See s_fwd_in_drops.
static err_t chain_input(netif_input_fn orig, struct pbuf *p, struct netif *inp, bool is_wifi)
{
    err_t err = orig(p, inp);
    if (err != ERR_OK) {
        s_fwd_in_drops[is_wifi ? 1 : 0]++;
    }
    return err;
}

// Chain to the port's real transmit and count what it refuses. See
// s_fwd_out_drops.
static err_t chain_output(netif_linkoutput_fn orig, struct netif *netif, struct pbuf *p,
                           bool is_wifi)
{
    err_t err = orig(netif, p);
    if (err != ERR_OK) {
        s_fwd_out_drops[is_wifi ? 1 : 0]++;
    }
    return err;
}

// RX wrapper (client -> bridge, uplink). Installed in place of the input
// pointer the bridge glue already assigned, so we chain to that, not to
// the raw driver input.
static err_t traffic_input_wrapper(struct pbuf *p, struct netif *inp, bool is_wifi,
                                    netif_input_fn orig)
{
    // p->len is only the first pbuf segment's length; a chained pbuf can have
    // the eth/IP headers split across segments even though p->tot_len covers
    // them. Fast-path the common single-segment case, and fall back to
    // pbuf_copy_partial() (checked against tot_len) for the rare chained one
    // instead of silently dropping the accounting event for that frame.
    // Sized to reach the UDP ports past the IP header as well, so a client's
    // own DHCP traffic can be told apart from its real traffic below. The
    // thresholds still test only the eth+IP part, so a short IPv4 frame is
    // sniffed exactly as before - the extra bytes are read only if present.
    uint8_t hdrbuf[sizeof(struct eth_hdr) + IP_HLEN + 4];
    const size_t eth_ip_len = sizeof(struct eth_hdr) + IP_HLEN;
    struct eth_hdr *hdr;
    bool have_ip_hdr;
    size_t have_len = 0;
    if (p != NULL && p->len >= eth_ip_len) {
        hdr = (struct eth_hdr *)p->payload;
        have_ip_hdr = true;
        have_len = p->len;
    } else if (p != NULL && p->tot_len >= eth_ip_len) {
        have_len = (p->tot_len < sizeof(hdrbuf)) ? p->tot_len : sizeof(hdrbuf);
        pbuf_copy_partial(p, hdrbuf, have_len, 0);
        hdr = (struct eth_hdr *)hdrbuf;
        have_ip_hdr = true;
    } else if (p != NULL && p->len >= 12) {
        // Header-only frame shorter than eth+IP (e.g. ARP): MAC is still
        // readable from the first segment, just skip IP sniffing below.
        hdr = (struct eth_hdr *)p->payload;
        have_ip_hdr = false;
    } else {
        return chain_input(orig, p, inp, is_wifi);
    }

    if (mac_is_client(hdr->src.addr)) {
        // Sniff the source IP straight off the client's own packet when
        // possible - some clients re-address themselves after DHCP
        // without ever renewing the lease, so the DHCP lease table
        // client_track_tick() polls can go stale while this can't.
        esp_ip4_addr_t src_ip;
        const esp_ip4_addr_t *src_ip_p = NULL;
        bool is_dhcp = false;
        if (have_ip_hdr && hdr->type == PP_HTONS(ETHTYPE_IP)) {
            struct ip_hdr *iph = (struct ip_hdr *)((uint8_t *)hdr + sizeof(struct eth_hdr));
            is_dhcp = is_dhcp_packet(iph, have_len - sizeof(struct eth_hdr));
            if (!is_dhcp) {
                src_ip.addr = iph->src.addr;
                src_ip_p = &src_ip;
            }
        }
        traffic_event_t ev = {
            .is_wifi = is_wifi,
            .bytes = p->tot_len,
            .is_uplink = true,
            .observed_ip = { .addr = 0 },
        };
        memcpy(ev.mac, hdr->src.addr, 6);
        if (src_ip_p != NULL) {
            ev.observed_ip = *src_ip_p;
        }
        // Timeout 0 is load-bearing, not a tuning knob: traffic
        // forwarding must never block on accounting, so a full queue
        // means drop-and-count here, never wait.
        if (xQueueSend(s_traffic_q, &ev, 0) != pdTRUE) {
            s_traffic_drops++;
        }

        // Cut off a client holding somebody else's address - but only after the
        // event above is queued. Observing it is what lets the quarantine be
        // lifted again: a device moved back onto an address of its own is
        // recognised from this same sniffed source IP, and dropping the frame
        // before recording it would leave nothing able to notice.
        //
        // DHCP is the deliberate hole in this. A device reconfigured from a
        // static address to DHCP has to be able to ask for one, or the
        // quarantine is a trap it cannot be fixed out of over the network.
        //
        // ARP is *not* let through, and that is the point rather than an
        // oversight: a gratuitous ARP or an ARP reply from the second device is
        // the actual mechanism by which a duplicate address breaks everyone
        // else's cache. Recovery does not need it - a client with no address
        // does DHCP entirely in broadcast.
        if (!is_dhcp && mac_is_quarantined(hdr->src.addr)) {
            s_q_drops[0]++;
            // Both drivers free the pbuf themselves when input returns anything
            // but ERR_OK (esp_netif/lwip/netif/{ethernetif,wlanif}.c), so this
            // frees and reports success instead, keeping them on the path they
            // take for a frame that was delivered. For WiFi the pbuf is a
            // custom one from esp_pbuf_allocate() whose free callback hands the
            // driver's L2 buffer back, so this is the complete release.
            pbuf_free(p);
            return ERR_OK;
        }
    }
    return chain_input(orig, p, inp, is_wifi);
}

static err_t eth_input_wrapper(struct pbuf *p, struct netif *inp)
{
    return traffic_input_wrapper(p, inp, false, s_eth_orig_input);
}

static err_t wifi_input_wrapper(struct pbuf *p, struct netif *inp)
{
    return traffic_input_wrapper(p, inp, true, s_wifi_orig_input);
}

// Whether a downlink frame is DHCP, read the long way round because the TX
// wrapper only pulls a 12-byte header window in the normal case.
//
// Only ever called for a frame already known to be headed for a quarantined
// MAC, so the extra pbuf_copy_partial() is paid on a path that is by definition
// not carrying real traffic. The ordinary downlink costs nothing.
static bool frame_is_dhcp(struct pbuf *p)
{
    uint8_t buf[sizeof(struct eth_hdr) + IP_HLEN + 4];
    if (p == NULL || p->tot_len < sizeof(struct eth_hdr) + IP_HLEN) {
        return false;
    }
    size_t len = (p->tot_len < sizeof(buf)) ? p->tot_len : sizeof(buf);
    if (pbuf_copy_partial(p, buf, len, 0) != len) {
        return false;
    }
    const struct eth_hdr *hdr = (const struct eth_hdr *)buf;
    if (hdr->type != PP_HTONS(ETHTYPE_IP)) {
        return false;
    }
    return is_dhcp_packet((const struct ip_hdr *)(buf + sizeof(struct eth_hdr)),
                          len - sizeof(struct eth_hdr));
}

// TX wrapper (bridge -> client, downlink). Broadcast/multicast frames are
// deliberately not attributed to any single client.
static err_t traffic_output_wrapper(struct netif *netif, struct pbuf *p, bool is_wifi,
                                     netif_linkoutput_fn orig)
{
    // See traffic_input_wrapper() for why this can't just check p->len.
    uint8_t hdrbuf[12];
    struct eth_hdr *hdr;
    if (p != NULL && p->len >= sizeof(hdrbuf)) {
        hdr = (struct eth_hdr *)p->payload;
    } else if (p != NULL && p->tot_len >= sizeof(hdrbuf)) {
        pbuf_copy_partial(p, hdrbuf, sizeof(hdrbuf), 0);
        hdr = (struct eth_hdr *)hdrbuf;
    } else {
        return chain_output(orig, netif, p, is_wifi);
    }

    if (mac_is_client(hdr->dest.addr)) {
        traffic_event_t ev = {
            .is_wifi = is_wifi,
            .bytes = p->tot_len,
            .is_uplink = false,
            .observed_ip = { .addr = 0 },
        };
        memcpy(ev.mac, hdr->dest.addr, 6);
        // See traffic_input_wrapper() - timeout 0 here for the same reason.
        if (xQueueSend(s_traffic_q, &ev, 0) != pdTRUE) {
            s_traffic_drops++;
        }

        // The other half of the cut-off. DHCP has to survive in this direction
        // too, or the unicast ACK to a client that is renewing never lands and
        // the escape hatch only half works. The MAC test comes first so the
        // wider header read behind frame_is_dhcp() only ever happens for a
        // device that is already being dropped.
        if (mac_is_quarantined(hdr->dest.addr) && !frame_is_dhcp(p)) {
            s_q_drops[1]++;
            // No pbuf_free() here, unlike the RX side: linkoutput never owns
            // what it is handed. bridgeif_input() frees this whatever
            // bridgeif_send_to_ports() reports - see s_fwd_out_drops above.
            return ERR_OK;
        }
    }
    return chain_output(orig, netif, p, is_wifi);
}

static err_t eth_output_wrapper(struct netif *netif, struct pbuf *p)
{
    return traffic_output_wrapper(netif, p, false, s_eth_orig_linkoutput);
}

static err_t wifi_output_wrapper(struct netif *netif, struct pbuf *p)
{
    return traffic_output_wrapper(netif, p, true, s_wifi_orig_linkoutput);
}

static void on_eth_port_started(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (s_eth_orig_input != NULL) {
        return; // already hooked
    }
    struct netif *nif = (struct netif *)esp_netif_get_netif_impl(s_eth_netif);
    if (nif == NULL) {
        ESP_LOGW(TAG, "eth netif impl not ready yet");
        return;
    }
    s_eth_orig_input = nif->input;
    s_eth_orig_linkoutput = nif->linkoutput;
    nif->input = eth_input_wrapper;
    nif->linkoutput = eth_output_wrapper;
    ESP_LOGI(TAG, "eth traffic accounting hooks installed");
}

static void on_wifi_port_started(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (s_wifi_orig_input != NULL) {
        return; // already hooked
    }
    struct netif *nif = (struct netif *)esp_netif_get_netif_impl(s_wifi_netif);
    if (nif == NULL) {
        ESP_LOGW(TAG, "wifi netif impl not ready yet");
        return;
    }
    s_wifi_orig_input = nif->input;
    s_wifi_orig_linkoutput = nif->linkoutput;
    nif->input = wifi_input_wrapper;
    nif->linkoutput = wifi_output_wrapper;
    ESP_LOGI(TAG, "wifi traffic accounting hooks installed");
}

// Fired by the DHCP server (on the bridge netif) each time it hands out or
// renews a lease - the one place a client's self-reported hostname (DHCP
// option 12) is available. Requires CONFIG_LWIP_DHCPS_REPORT_CLIENT_HOSTNAME,
// already enabled in sdkconfig.
static void on_client_ip_assigned(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_assigned_ip_to_client_t *evt = (ip_event_assigned_ip_to_client_t *)data;
    if (evt->esp_netif != s_br_netif) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    client_entry_t *e = find_or_create_locked(evt->mac);
    if (e) {
        // Only bootstrap from this event, never override: some clients keep
        // renewing their original DHCP lease in the background (e.g. after
        // applying their own static IP without stopping the DHCP client
        // task), which would otherwise re-fire this event with the stale
        // leased address and stomp a correctly observed real IP.
        if (e->ip.addr == 0) {
            update_client_ip_locked(e, evt->ip, "leased by the DHCP server");
        }
        if (evt->hostname[0] != '\0') {
            strlcpy(e->name, evt->hostname, sizeof(e->name));
        }
    }
    xSemaphoreGive(s_mutex);
}

// Disconnect reasons that actually turn up on a soft-AP. Anything else prints
// as a bare number: the full 802.11 list is long, mostly concerns station-mode
// roaming, and transcribing all of it would go stale against ESP-IDF's enum
// for no benefit.
static const char *disconnect_reason_str(uint16_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:                return "auth expired";
    case WIFI_REASON_AUTH_LEAVE:                 return "deauthenticated";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY: return "idle too long";
    case WIFI_REASON_ASSOC_TOOMANY:              return "AP full";
    case WIFI_REASON_ASSOC_LEAVE:                return "left";
    case WIFI_REASON_MIC_FAILURE:                return "MIC failure";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:     return "wrong password, or handshake timed out";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:   return "group key update timed out";
    case WIFI_REASON_STA_LEAVING:                return "leaving";
    case WIFI_REASON_TIMEOUT:                    return "timeout";
    // The one this device has a history with - see the PMF note in
    // wifi_cfg_apply(), where turning 802.11w off is what stopped it.
    case WIFI_REASON_SA_QUERY_TIMEOUT:           return "SA query timeout";
    default:                                     return NULL;
    }
}

// Describes a station by the address and name the client table already knows,
// so a join line says which device it was and not only which MAC. Writes into
// a caller-supplied buffer and returns it, for use inline in a log call.
static const char *describe_client(const uint8_t mac[6], char *buf, size_t len)
{
    char ip_str[20] = "no address yet";
    char name[CLIENT_TRACK_NAME_MAX_LEN] = "";

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < CLIENT_TRACK_MAX_CLIENTS; i++) {
        client_entry_t *e = &s_clients[i];
        if (e->active && memcmp(e->mac, mac, 6) == 0) {
            if (e->ip.addr != 0) {
                esp_ip4addr_ntoa(&e->ip, ip_str, sizeof(ip_str));
            }
            strlcpy(name, e->name, sizeof(name));
            break;
        }
    }
    xSemaphoreGive(s_mutex);

    if (name[0] != '\0') {
        snprintf(buf, len, "%s, %s", ip_str, name);
    } else {
        snprintf(buf, len, "%s", ip_str);
    }
    return buf;
}

// The WiFi driver logs its own join and leave lines, but they are internal
// ("<ba-add>idx:2 (ifx:1, ...)"), sit at INFO under the noisy "wifi" tag, and
// identify a station only by MAC and AID. These say plainly which client came
// or went, so a re-association loop reads as what it is rather than having to
// be inferred from block-ack churn.
static void on_wifi_sta_connected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    wifi_event_ap_staconnected_t *evt = (wifi_event_ap_staconnected_t *)data;
    char who[64];
    ESP_LOGI(TAG, "wifi station joined: %02x:%02x:%02x:%02x:%02x:%02x (aid %u) - %s",
             evt->mac[0], evt->mac[1], evt->mac[2], evt->mac[3], evt->mac[4], evt->mac[5],
             (unsigned)evt->aid, describe_client(evt->mac, who, sizeof(who)));
}

static void on_wifi_sta_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    wifi_event_ap_stadisconnected_t *evt = (wifi_event_ap_stadisconnected_t *)data;

    char reason_buf[16];
    const char *why = disconnect_reason_str(evt->reason);
    if (why == NULL) {
        snprintf(reason_buf, sizeof(reason_buf), "reason %u", (unsigned)evt->reason);
        why = reason_buf;
    }

    char who[64];
    ESP_LOGI(TAG, "wifi station left: %02x:%02x:%02x:%02x:%02x:%02x (aid %u) - %s (%s)",
             evt->mac[0], evt->mac[1], evt->mac[2], evt->mac[3], evt->mac[4], evt->mac[5],
             (unsigned)evt->aid, why, describe_client(evt->mac, who, sizeof(who)));
}

// ---- duplicate-address quarantine, decision half ----

#define Q_ARM_TICKS      3   // consecutive 1Hz confirmations before cutting anyone off
#define Q_DISARM_TICKS  10   // consecutive clean ticks before letting them back
#define Q_FORGET_TICKS  30   // ...and before the record itself is dropped
#define Q_LIVENESS_US    (30LL * 1000000)
#define Q_LOG_REPEAT_US  (5LL * 60 * 1000000)

typedef struct {
    bool     used;
    uint8_t  mac[6];    // the one being cut off
    uint8_t  peer[6];   // the one keeping the address
    uint32_t ip;        // contested address, network byte order
    uint8_t  confirm;   // consecutive ticks this has been visible
    uint8_t  miss;      // consecutive ticks it has not
    bool     armed;
    int64_t  armed_us;
    int64_t  logged_us; // last time this was announced, to cap repeats
} conflict_t;

static conflict_t s_conflicts[QUARANTINE_MAX];
// MACs an operator has pardoned from the console. Not persisted: a pardon is a
// judgement about the network as it is right now, and carrying it across a
// reboot would silently disable the check for a device nobody remembers
// excusing.
static uint8_t s_q_exempt[QUARANTINE_MAX][6];
static int s_q_exempt_count;
// The kill switch. Clearing it stops the dropping only - detection, logging and
// everything the UI reports carry on, so the diagnosis stays available when the
// enforcement has to be called off.
static bool s_q_enforce = true;

// The address a lease entry means a device is actually on. Same precedence as
// dhcp_server_lookup_ip(): what the client was seen using beats what it was
// leased. An expired lease claims nothing - that address is exactly what gets
// recycled - so it reports 0.
static uint32_t lease_effective_ip(const dhcp_lease_info_t *l)
{
    if (l->observed_ip.addr != 0) {
        return l->observed_ip.addr;
    }
    if (l->expires_in_s > 0) {
        return l->ip.addr;
    }
    return 0;
}

// Caller must hold s_mutex.
static client_entry_t *find_locked(const uint8_t *mac)
{
    for (int i = 0; i < CLIENT_TRACK_MAX_CLIENTS; i++) {
        if (s_clients[i].active && memcmp(s_clients[i].mac, mac, 6) == 0) {
            return &s_clients[i];
        }
    }
    return NULL;
}

// Of two MACs found on one address, true if b is the later arrival - the one
// that loses it. First device on the address keeps it.
//
// Both times come from the client's own traffic (see account_traffic), so this
// compares like with like: two moments this bridge actually watched a device
// using the address. Nothing is inferred from flash or the ARP cache, because
// an address from those says only what this device believed on the last boot,
// and an earlier belief is not an earlier arrival.
//
// A 0 therefore means "never seen there", not "seen there long ago", and cannot
// win: quarantine_rebuild() refuses to arm at all unless both sides have a real
// time, so the 0 cases here only run while a conflict is still being confirmed.
// Ordering them at all is just so the record holds a sensible pair in the
// meantime.
//
// The MAC tie-break is unreachable in a confirmed conflict for the same reason
// - two devices cannot be first-seen in the same microsecond - and exists so
// the function is total. Lower MAC keeps the address, which is arbitrary but
// arbitrary in the same direction every time.
static bool loser_is_b(const client_entry_t *ea, const client_entry_t *eb,
                        const uint8_t *ma, const uint8_t *mb)
{
    int64_t ta = (ea != NULL) ? ea->ip_since_us : 0;
    int64_t tb = (eb != NULL) ? eb->ip_since_us : 0;

    if (ta != tb) {
        // Whoever has no confirmed sighting is the one to give way, and when
        // both have one the later of the two arrived second.
        if (ta == 0) {
            return false;
        }
        if (tb == 0) {
            return true;
        }
        return tb > ta;
    }
    return memcmp(mb, ma, 6) > 0;
}

// Caller must hold s_mutex.
static bool mac_is_exempt_locked(const uint8_t *mac)
{
    for (int i = 0; i < s_q_exempt_count; i++) {
        if (memcmp(s_q_exempt[i], mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

// Caller must hold s_mutex. Finds the record for this address and this pair of
// MACs, in either order.
static conflict_t *conflict_find_locked(uint32_t ip, const uint8_t *m1, const uint8_t *m2)
{
    for (int i = 0; i < QUARANTINE_MAX; i++) {
        conflict_t *c = &s_conflicts[i];
        if (!c->used || c->ip != ip) {
            continue;
        }
        if ((memcmp(c->mac, m1, 6) == 0 && memcmp(c->peer, m2, 6) == 0) ||
            (memcmp(c->mac, m2, 6) == 0 && memcmp(c->peer, m1, 6) == 0)) {
            return c;
        }
    }
    return NULL;
}

// Caller must hold s_mutex.
static conflict_t *conflict_alloc_locked(void)
{
    for (int i = 0; i < QUARANTINE_MAX; i++) {
        if (!s_conflicts[i].used) {
            return &s_conflicts[i];
        }
    }
    return NULL;
}

static void log_conflict(const conflict_t *c, bool armed)
{
    esp_ip4_addr_t pretty = { .addr = c->ip };
    char ip_str[16];
    esp_ip4addr_ntoa(&pretty, ip_str, sizeof(ip_str));
    if (armed) {
        ESP_LOGW(TAG, "duplicate address %s: %02x:%02x:%02x:%02x:%02x:%02x was there first, "
                      "so %02x:%02x:%02x:%02x:%02x:%02x is being cut off until it moves",
                 ip_str,
                 c->peer[0], c->peer[1], c->peer[2], c->peer[3], c->peer[4], c->peer[5],
                 c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5]);
    } else {
        ESP_LOGW(TAG, "duplicate address %s resolved - %02x:%02x:%02x:%02x:%02x:%02x "
                      "is being carried again", ip_str,
                 c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5]);
    }
}

// Rebuilds the conflict set from scratch once a second and republishes it.
//
// Rebuilding wholesale rather than tracking changes is what makes this
// self-clearing: a conflict ends because a lease expired, because an entry was
// reclaimed out of the lease table, or because a device moved to another
// address, and none of those three announce themselves. Recomputing sees all of
// them for free.
//
// Called once per second from client_track_task, and from nowhere else - the
// hysteresis counters below are all in units of that tick.
static void quarantine_rebuild(void)
{
    // Static rather than on the stack: 32 lease entries runs to well over a
    // kilobyte, and client_track_task's 4096 bytes already carry pairs[16],
    // macs[16][6] and a wifi_sta_list_t. Only this task ever touches it.
    static dhcp_lease_info_t leases[DHCP_SERVER_MAX_LEASES];
    int n = dhcp_server_get_leases(leases, DHCP_SERVER_MAX_LEASES);
    int64_t now = esp_timer_get_time();

    // Taking the dhcp_server snapshot above happens outside this lock, but
    // holding it across the dhcp_server calls further down would be safe too:
    // this file's mutex before that one is the established order, and
    // dhcp_server.c never calls back. See account_traffic().
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool seen[QUARANTINE_MAX] = { false };

    for (int i = 0; i < n; i++) {
        uint32_t addr = lease_effective_ip(&leases[i]);
        if (addr == 0) {
            continue;
        }
        for (int j = i + 1; j < n; j++) {
            if (lease_effective_ip(&leases[j]) != addr) {
                continue;
            }

            conflict_t *c = conflict_find_locked(addr, leases[i].mac, leases[j].mac);
            if (c == NULL) {
                c = conflict_alloc_locked();
                if (c == NULL) {
                    continue;   // more simultaneous conflicts than there are slots
                }
                memset(c, 0, sizeof(*c));
                c->used = true;
                c->ip = addr;
            }

            // Who loses is settled once and then left alone for as long as the
            // quarantine holds. Re-deciding every tick would let the answer
            // alternate between the two devices as their timestamps move, which
            // is precisely the intermittent behaviour this exists to end.
            if (!c->armed) {
                client_entry_t *ei = find_locked(leases[i].mac);
                client_entry_t *ej = find_locked(leases[j].mac);
                int loser = loser_is_b(ei, ej, leases[i].mac, leases[j].mac) ? j : i;
                int winner = (loser == j) ? i : j;
                memcpy(c->mac, leases[loser].mac, 6);
                memcpy(c->peer, leases[winner].mac, 6);
            }

            seen[c - s_conflicts] = true;
            c->miss = 0;
            if (c->confirm < Q_ARM_TICKS) {
                c->confirm++;
            }
        }
    }

    uint8_t armed_macs[QUARANTINE_MAX][6];
    int armed_count = 0;

    for (int i = 0; i < QUARANTINE_MAX; i++) {
        conflict_t *c = &s_conflicts[i];
        if (!c->used) {
            continue;
        }

        if (!seen[i]) {
            c->confirm = 0;
            if (c->miss < 255) {
                c->miss++;
            }
            if (c->armed && c->miss >= Q_DISARM_TICKS) {
                c->armed = false;
                log_conflict(c, false);
            }
            if (c->miss >= Q_FORGET_TICKS) {
                c->used = false;
                continue;
            }
        } else if (!c->armed && c->confirm >= Q_ARM_TICKS) {
            // Liveness gates arming and nothing else. A lease with time left on
            // it is not evidence that its holder is on the network, and cutting
            // a device off for colliding with a machine that was switched off an
            // hour ago is the largest way this could misfire. Both parties have
            // to have been heard from recently for the collision to be real.
            //
            // It also covers a client that rejoins with a new MAC and keeps its
            // address: the old MAC stops transmitting the moment the new one
            // starts, so it falls outside the window before this can arm.
            client_entry_t *loser = find_locked(c->mac);
            client_entry_t *winner = find_locked(c->peer);
            bool both_live = loser != NULL && winner != NULL &&
                             (now - loser->last_seen_us) < Q_LIVENESS_US &&
                             (now - winner->last_seen_us) < Q_LIVENESS_US;
            // And both must have been *seen on the contested address by their
            // own traffic*, not merely believed to be on it. The lease table
            // remembers a self-assigned address across reboots, so an entry can
            // name a device that moved away months ago, or one that is present
            // but sitting on a different address - and a client that only ever
            // ARPs never corrects the record, because the address is sniffed
            // from IPv4 headers.
            //
            // Without this a device quietly using its own address gets cut off
            // in favour of a ghost, which is the worst outcome this code can
            // produce: a working machine taken off the network to protect an
            // address nobody is using.
            bool both_confirmed = both_live &&
                                  loser->ip_since_us != 0 && winner->ip_since_us != 0 &&
                                  loser->ip.addr == c->ip && winner->ip.addr == c->ip;
            if (both_confirmed && !mac_is_exempt_locked(c->mac)) {
                c->armed = true;
                c->armed_us = now;
                // Announced on the edge, and at most once every few minutes for
                // the same pair. The loser's lease entry can be reclaimed and
                // then rebuilt by dhcp_server_note_observed_ip() the next time
                // it speaks, which would otherwise turn a single fault into a
                // repeating log line.
                if (c->logged_us == 0 || (now - c->logged_us) > Q_LOG_REPEAT_US) {
                    c->logged_us = now;
                    log_conflict(c, true);
                }
            }
        }

        if (c->armed && armed_count < QUARANTINE_MAX) {
            memcpy(armed_macs[armed_count], c->mac, 6);
            armed_count++;
        }
    }

    // With enforcement off both consumers are handed an empty set, so the
    // switch means one thing: the bridge takes no action at all. Not just no
    // dropping - the DHCP server also stops discounting the offender's claim
    // and stops NAKing it onto another address, which is just as much an action
    // and would be a surprising thing to still be happening after somebody
    // turned this off. Detection is untouched: everything above still ran, and
    // s_conflicts is what the console and the UI report from.
    //
    // Publishing empty also keeps the hot path free of any notion of the
    // switch - it still exits on the single s_q_any load.
    int enforced = s_q_enforce ? armed_count : 0;
    quarantine_publish(armed_macs, enforced);

    xSemaphoreGive(s_mutex);

    // Outside the lock, and unconditional: the server has to be told when the
    // set empties as well as when it fills.
    dhcp_server_set_quarantined(armed_macs, enforced);
}

static void client_track_tick(void)
{
    int64_t now = esp_timer_get_time();
    uint8_t macs[CLIENT_TRACK_MAX_CLIENTS][6];
    int n = 0;
    uint32_t rx_pps_sum = 0, tx_pps_sum = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < CLIENT_TRACK_MAX_CLIENTS; i++) {
        client_entry_t *e = &s_clients[i];
        if (!e->active) {
            continue;
        }
        if (now - e->last_seen_us > CLIENT_AGE_OUT_US) {
            e->active = false;
            continue;
        }
        e->rx_bps = e->rx_bytes - e->rx_bytes_prev;
        e->tx_bps = e->tx_bytes - e->tx_bytes_prev;
        e->rx_bytes_prev = e->rx_bytes;
        e->tx_bytes_prev = e->tx_bytes;
        e->rx_pps = e->rx_pkts - e->rx_pkts_prev;
        e->tx_pps = e->tx_pkts - e->tx_pkts_prev;
        e->rx_pkts_prev = e->rx_pkts;
        e->tx_pkts_prev = e->tx_pkts;
        rx_pps_sum += e->rx_pps;
        tx_pps_sum += e->tx_pps;
        memcpy(macs[n], e->mac, 6);
        n++;
    }
    s_net_rx_pps = rx_pps_sum;
    s_net_tx_pps = tx_pps_sum;
    xSemaphoreGive(s_mutex);

    // Resolve IPs outside the lock: the DHCP server's lease table first, ARP
    // cache as a fallback for statically-addressed clients. This is only a
    // bootstrap for clients we haven't directly observed traffic from yet - once we
    // have a real IP (via on_client_ip_assigned or sniffed uplink traffic),
    // it takes priority, since the lease table can go stale for clients
    // that re-address themselves without renewing.
    esp_netif_pair_mac_ip_t pairs[CLIENT_TRACK_MAX_CLIENTS];
    bool from_lease[CLIENT_TRACK_MAX_CLIENTS];   // which of the two answered, for the log
    for (int i = 0; i < n; i++) {
        memcpy(pairs[i].mac, macs[i], 6);
        pairs[i].ip.addr = 0;
        from_lease[i] = false;
    }
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            // dhcp_server.c rather than esp_netif_dhcps_get_clients_by_mac():
            // that one only answers for ESP-IDF's own server, which this
            // device no longer runs (see setup_bridge() in main.c).
            from_lease[i] = dhcp_server_lookup_ip(pairs[i].mac, &pairs[i].ip);
            if (!from_lease[i]) {
                esp_netif_arp_get_client_by_mac(s_br_netif, &pairs[i]);
            }
        }
    }

    wifi_sta_list_t sta_list;
    bool have_sta_list = (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < n; i++) {
        client_entry_t *e = find_or_create_locked(macs[i]);
        if (e && e->ip.addr == 0) {
            update_client_ip_locked(e, pairs[i].ip,
                                    from_lease[i] ? "from the lease table" : "from the ARP cache");
        }
    }
    if (have_sta_list) {
        for (int i = 0; i < sta_list.num; i++) {
            client_entry_t *e = find_or_create_locked(sta_list.sta[i].mac);
            if (e) {
                e->is_wifi = true;
                e->rssi = sta_list.sta[i].rssi;
            }
        }
    }
    xSemaphoreGive(s_mutex);

    // Last, so it works from the addresses this tick just resolved rather than
    // from last second's. Takes the lock itself.
    quarantine_rebuild();
}

// Runs every CLIENT_TRACK_HISTORY_PERIOD_MS (much more often than the 1Hz
// client_track_tick()) to record a fine-grained rx/tx sample per client, so a
// page-level traffic graph can show bursts shorter than one second. Cheap
// enough (a handful of clients, plain subtraction) to run unconditionally for
// every tracked slot rather than only for a client someone is watching.
static void client_track_fast_sample(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < CLIENT_TRACK_MAX_CLIENTS; i++) {
        client_entry_t *e = &s_clients[i];
        if (!e->active) {
            continue;
        }
        uint32_t rx_delta = e->rx_bytes - e->rx_bytes_prev_fast;
        uint32_t tx_delta = e->tx_bytes - e->tx_bytes_prev_fast;
        e->rx_bytes_prev_fast = e->rx_bytes;
        e->tx_bytes_prev_fast = e->tx_bytes;
        uint32_t rx_pkt_delta = e->rx_pkts - e->rx_pkts_prev_fast;
        uint32_t tx_pkt_delta = e->tx_pkts - e->tx_pkts_prev_fast;
        e->rx_pkts_prev_fast = e->rx_pkts;
        e->tx_pkts_prev_fast = e->tx_pkts;
        int idx = e->hist_seq % CLIENT_TRACK_HISTORY_LEN;
        e->hist_rx[idx] = rx_delta * (1000 / CLIENT_TRACK_HISTORY_PERIOD_MS);
        e->hist_tx[idx] = tx_delta * (1000 / CLIENT_TRACK_HISTORY_PERIOD_MS);
        e->hist_rx_pps[idx] = rx_pkt_delta * (1000 / CLIENT_TRACK_HISTORY_PERIOD_MS);
        e->hist_tx_pps[idx] = tx_pkt_delta * (1000 / CLIENT_TRACK_HISTORY_PERIOD_MS);
        e->hist_seq++;
    }
    xSemaphoreGive(s_mutex);
}

static void client_track_task(void *arg)
{
    int fast_count = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CLIENT_TRACK_HISTORY_PERIOD_MS));
        client_track_fast_sample();
        if (++fast_count >= FAST_TICKS_PER_SLOW_TICK) {
            fast_count = 0;
            client_track_tick();
        }
    }
}

// Drains traffic_event_t entries queued by the eth/wifi RX/TX hot path and
// applies them via the original (unchanged) account_traffic(). Keeping this
// as its own task, pinned to the opposite core from the networking hot path,
// means the mutex/scan work in account_traffic() never runs on the same core
// that's servicing packet forwarding.
static void traffic_account_task(void *arg)
{
    traffic_event_t ev;
    while (1) {
        if (xQueueReceive(s_traffic_q, &ev, portMAX_DELAY) == pdTRUE) {
            account_traffic(ev.mac, ev.is_wifi, ev.bytes, ev.is_uplink,
                             ev.observed_ip.addr != 0 ? &ev.observed_ip : NULL);
        }
    }
}

esp_err_t client_track_init(esp_netif_t *eth_netif, esp_netif_t *wifi_netif,
                             esp_netif_t *br_netif, const uint8_t *bridge_mac)
{
    s_eth_netif = eth_netif;
    s_wifi_netif = wifi_netif;
    s_br_netif = br_netif;
    memcpy(s_bridge_mac, bridge_mac, 6);

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_traffic_q = xQueueCreate(TRAFFIC_EVENT_QUEUE_DEPTH, sizeof(traffic_event_t));
    if (s_traffic_q == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Registered after esp_netif_br_glue's own handlers for these same
    // events (setup_bridge() ran first), so esp_event's in-order dispatch
    // guarantees the bridge has already replaced netif->input by the time
    // our handler runs.
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_START,
                                                &on_eth_port_started, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START,
                                                &on_wifi_port_started, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                                &on_client_ip_assigned, NULL));

    // Purely for the log - neither handler touches the client table, which
    // learns about stations from their traffic and from esp_wifi_ap_get_sta_list()
    // in the tick. Registered here rather than in main.c because saying which
    // client joined needs the table this file owns.
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                                &on_wifi_sta_connected, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                                &on_wifi_sta_disconnected, NULL));

    BaseType_t ok = xTaskCreatePinnedToCore(client_track_task, "client_track", 4096,
                                             NULL, tskIDLE_PRIORITY + 2, NULL, 1);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    // Priority idle+3: higher than client_track_task (idle+2) so it
    // preferentially drains the traffic queue over that slower housekeeping
    // work, but deliberately far below the actual traffic-critical tasks
    // (Ethernet RX ~15, lwIP tcpip 18, WiFi's own driver task higher still)
    // so accounting can never preempt or delay real packet forwarding.
    BaseType_t acct_ok = xTaskCreatePinnedToCore(traffic_account_task, "traffic_acct", 3072,
                                                  NULL, tskIDLE_PRIORITY + 3, NULL, 1);
    return acct_ok == pdPASS ? ESP_OK : ESP_FAIL;
}

uint32_t client_track_get_traffic_drops(void)
{
    return s_traffic_drops;
}

// Runs in the WiFi driver's own context, once per transmitted frame, so it does
// the least it possibly can: two increments and no lock. Racy for the same
// reason as every other counter in this file.
static void wifi_tx_done_cb(uint8_t ifidx, uint8_t *data, uint16_t *data_len, bool txStatus)
{
    (void)ifidx;
    (void)data;
    (void)data_len;
    s_wifi_tx_total++;
    if (!txStatus) {
        s_wifi_tx_failed++;
    }
}

esp_err_t client_track_wifi_txdone_init(void)
{
    // Has to be called after esp_wifi_start() - the API returns
    // ESP_ERR_WIFI_NOT_STARTED otherwise, which is why this is not folded into
    // client_track_init() with the rest of the hooks.
    return esp_wifi_set_tx_done_cb(wifi_tx_done_cb);
}

void client_track_get_wifi_tx(uint32_t *total, uint32_t *failed)
{
    *total = s_wifi_tx_total;
    *failed = s_wifi_tx_failed;
}

void client_track_get_forward_drops(client_track_fwd_drops_t *out)
{
    out->eth_in = s_fwd_in_drops[0];
    out->wifi_in = s_fwd_in_drops[1];
    out->eth_out = s_fwd_out_drops[0];
    out->wifi_out = s_fwd_out_drops[1];
}

void client_track_get_traffic_pps(uint32_t *rx_pps, uint32_t *tx_pps)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *rx_pps = s_net_rx_pps;
    *tx_pps = s_net_tx_pps;
    xSemaphoreGive(s_mutex);
}

int client_track_get_conflicts(client_track_conflict_t *out, int max)
{
    if (s_mutex == NULL) {
        return 0;
    }
    int64_t now = esp_timer_get_time();
    int n = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < QUARANTINE_MAX && n < max; i++) {
        conflict_t *c = &s_conflicts[i];
        if (!c->used) {
            continue;
        }
        memcpy(out[n].mac, c->mac, 6);
        memcpy(out[n].peer_mac, c->peer, 6);
        out[n].ip.addr = c->ip;
        out[n].armed = c->armed;
        out[n].dropping = c->armed && s_q_enforce;
        out[n].exempt = mac_is_exempt_locked(c->mac);
        out[n].armed_s = c->armed ? (uint32_t)((now - c->armed_us) / 1000000) : 0;
        n++;
    }
    xSemaphoreGive(s_mutex);
    return n;
}

int client_track_get_armed_conflicts(void)
{
    if (s_mutex == NULL) {
        return 0;
    }
    int n = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < QUARANTINE_MAX; i++) {
        if (s_conflicts[i].used && s_conflicts[i].armed) {
            n++;
        }
    }
    xSemaphoreGive(s_mutex);
    return n;
}

void client_track_get_quarantine_drops(uint32_t *uplink, uint32_t *downlink)
{
    *uplink = s_q_drops[0];
    *downlink = s_q_drops[1];
}

bool client_track_quarantine_clear(const uint8_t mac[6])
{
    if (s_mutex == NULL) {
        return false;
    }
    bool done = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (mac == NULL) {
        s_q_exempt_count = 0;
        done = true;
    } else if (!mac_is_exempt_locked(mac) && s_q_exempt_count < QUARANTINE_MAX) {
        memcpy(s_q_exempt[s_q_exempt_count], mac, 6);
        s_q_exempt_count++;
        done = true;
    }

    if (done && mac != NULL) {
        // Disarm right away rather than waiting on the next rebuild. A pardon
        // is somebody saying this device is wanted back on the network now, and
        // a second of continued dropping would read as the command not working.
        for (int i = 0; i < QUARANTINE_MAX; i++) {
            conflict_t *c = &s_conflicts[i];
            if (c->used && c->armed && memcmp(c->mac, mac, 6) == 0) {
                c->armed = false;
            }
        }
        uint8_t armed_macs[QUARANTINE_MAX][6];
        int armed_count = 0;
        for (int i = 0; i < QUARANTINE_MAX; i++) {
            if (s_conflicts[i].used && s_conflicts[i].armed && armed_count < QUARANTINE_MAX) {
                memcpy(armed_macs[armed_count], s_conflicts[i].mac, 6);
                armed_count++;
            }
        }
        quarantine_publish(armed_macs, s_q_enforce ? armed_count : 0);
    }
    xSemaphoreGive(s_mutex);
    return done;
}

void client_track_quarantine_enforce(bool on)
{
    if (s_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_q_enforce = on;
    if (!on) {
        // Stop dropping this instant rather than at the next tick. The DHCP
        // server's half of it catches up within the second, which is soon
        // enough - it only bears on the next address it is asked for.
        quarantine_publish(NULL, 0);
    } else {
        uint8_t armed_macs[QUARANTINE_MAX][6];
        int armed_count = 0;
        for (int i = 0; i < QUARANTINE_MAX; i++) {
            if (s_conflicts[i].used && s_conflicts[i].armed && armed_count < QUARANTINE_MAX) {
                memcpy(armed_macs[armed_count], s_conflicts[i].mac, 6);
                armed_count++;
            }
        }
        quarantine_publish(armed_macs, armed_count);
    }
    xSemaphoreGive(s_mutex);
}

bool client_track_quarantine_enforcing(void)
{
    return s_q_enforce;
}

void client_track_get_snapshot(client_info_t *out, int max, int *count)
{
    int64_t now = esp_timer_get_time();
    int n = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < CLIENT_TRACK_MAX_CLIENTS && n < max; i++) {
        client_entry_t *e = &s_clients[i];
        if (!e->active) {
            continue;
        }
        client_info_t *o = &out[n++];
        memcpy(o->mac, e->mac, 6);
        o->ip = e->ip;
        o->is_wifi = e->is_wifi;
        o->rssi = e->rssi;
        o->rx_bps = e->rx_bps;
        o->tx_bps = e->tx_bps;
        o->rx_pps = e->rx_pps;
        o->tx_pps = e->tx_pps;
        int64_t age_us = now - e->last_seen_us;
        o->last_seen_s = age_us > 0 ? (uint32_t)(age_us / 1000000) : 0;
        strlcpy(o->name, e->name, sizeof(o->name));
    }
    xSemaphoreGive(s_mutex);

    *count = n;
}

bool client_track_get_history(const uint8_t mac[6], uint32_t since_seq, client_history_t *out)
{
    bool found = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < CLIENT_TRACK_MAX_CLIENTS; i++) {
        client_entry_t *e = &s_clients[i];
        if (!e->active || memcmp(e->mac, mac, 6) != 0) {
            continue;
        }
        found = true;

        // Unsigned subtraction: also correctly treats a since_seq from before
        // a reboot (hist_seq restarts at 0, so this underflows to a huge
        // value) the same as ordinary ring-buffer wraparound - both just mean
        // "more happened than we still have buffered".
        uint32_t avail = e->hist_seq - since_seq;
        bool reset = false;
        if (avail > CLIENT_TRACK_HISTORY_LEN) {
            avail = e->hist_seq < CLIENT_TRACK_HISTORY_LEN ? e->hist_seq : CLIENT_TRACK_HISTORY_LEN;
            reset = true;
        }

        out->seq = e->hist_seq;
        out->reset = reset;
        out->count = (int)avail;
        uint32_t start = e->hist_seq - avail;
        for (uint32_t k = 0; k < avail; k++) {
            int idx = (start + k) % CLIENT_TRACK_HISTORY_LEN;
            out->rx_bps[k] = e->hist_rx[idx];
            out->tx_bps[k] = e->hist_tx[idx];
            out->rx_pps[k] = e->hist_rx_pps[idx];
            out->tx_pps[k] = e->hist_tx_pps[idx];
        }
        break;
    }
    xSemaphoreGive(s_mutex);

    return found;
}
