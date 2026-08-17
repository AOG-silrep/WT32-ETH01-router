#include <string.h>
#include "wan.h"
#include "wan_cfg.h"
#include "wifi_cfg.h"
#include "esp_netif_net_stack.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip.h"     // IP_PROTO_UDP / _TCP / _ICMP
#include "lwip/prot/ip4.h"
#include "lwip/prot/icmp.h"   // ICMP_ER / ICMP_DUR / ICMP_TE / ICMP_ECHO
#include "lwip/lwip_napt.h"   // IP_NAPT_PORT_RANGE_START / _END
#include "netif/ethernet.h"

static const char *TAG = "wan";

// The LAN, duplicated from BRIDGE_IP/BRIDGE_NETMASK in main.c. Only used for the
// overlap test in on_got_ip(); if the addressing ever changes, this is one of
// the places to look, alongside the literal in syslog_cfg_validate().
#define LAN_IP   PP_HTONL(LWIP_MAKEU32(192, 168, 5, 1))
#define LAN_MASK PP_HTONL(LWIP_MAKEU32(255, 255, 255, 0))

#define BACKOFF_MIN_S      1
#define BACKOFF_MAX_S      60
// A network that will not have us is not worth a retry every minute for the rest
// of the device's life. Still retried, never given up on: the password may be
// corrected on the AP's side, and a bridge that needed a reboot to notice would
// be a bridge somebody has to walk out to a field to reach.
#define BACKOFF_REJECTED_S 300
#define AUTH_FAILS_BEFORE_SLOW 3

static esp_netif_t *s_br_netif;
static esp_netif_t *s_sta_netif;
static esp_timer_handle_t s_retry_timer;
static bool s_napt_on;
static uint32_t s_backoff_s = BACKOFF_MIN_S;
static uint8_t s_auth_fails;

static netif_input_fn      s_sta_orig_input;
static netif_linkoutput_fn s_sta_orig_linkoutput;

// Written only from event-handler context, read from anywhere. Scalar fields
// updated in place; nothing here is consistent as a group, and nothing needs to
// be - it is a status display, not a decision input. The datapath's decision
// input is s_active below.
static wan_status_t s_status;

// What the filter enforces, published as one immutable snapshot so the two
// wrappers - which run in the WiFi driver task and the tcpip task - never take a
// lock on the datapath. Only event-handler context writes: it fills the buffer
// that is *not* live, then publishes with a single aligned pointer store. A
// reader takes the pointer once and uses it throughout, so it can never observe
// a half-written rule set. Same discipline as traffic_output_wrapper()'s
// zero-timeout queue send in client_track.c: forwarding must never block on
// policy.
typedef struct {
    wan_port_rule_t ports[WAN_CFG_MAX_PORTS];
    uint8_t         nports;
    uint32_t        dns_ip;             // network order; 0 = none learnt
    bool            up;
} wan_rules_t;

static wan_rules_t s_rules[2];
static wan_rules_t *volatile s_active = &s_rules[0];

// Writers only. Readers on the datapath never touch this - that is the whole
// point of the snapshot. But there is more than one writer: the event task
// publishes on every state change, and the httpd worker or console task
// publishes through wan_ports_changed(). Without this, two writers can pick the
// same "next" buffer and interleave their fills, and the survivor is a rule set
// neither of them wrote.
static SemaphoreHandle_t s_publish_mutex;

static void publish_rules(bool up, uint32_t dns_ip)
{
    wan_cfg_t cfg;
    wan_cfg_get(&cfg);

    if (s_publish_mutex != NULL) {
        xSemaphoreTake(s_publish_mutex, portMAX_DELAY);
    }

    wan_rules_t *next = (s_active == &s_rules[0]) ? &s_rules[1] : &s_rules[0];
    memset(next, 0, sizeof(*next));
    memcpy(next->ports, cfg.ports, sizeof(next->ports));
    next->nports = (cfg.nports <= WAN_CFG_MAX_PORTS) ? cfg.nports : WAN_CFG_MAX_PORTS;
    next->dns_ip = dns_ip;
    next->up = up;

    s_active = next;

    if (s_publish_mutex != NULL) {
        xSemaphoreGive(s_publish_mutex);
    }
}

// ---------------------------------------------------------------------------
// The filter
// ---------------------------------------------------------------------------

// A readable window over the front of a frame. Same shape as
// traffic_input_wrapper()'s: p->len covers only the first segment, so fast-path
// the single-segment case and fall back to a copy for a chained pbuf rather than
// misreading a header that is split across segments.
//
// 38 bytes reaches the TCP/UDP port pair (and the ICMP type) past a 20-byte IP
// header, which is everything any rule below looks at.
#define WAN_HDR_WINDOW (sizeof(struct eth_hdr) + IP_HLEN + 4)

typedef struct {
    const struct eth_hdr *eth;
    const struct ip_hdr  *ip;    // NULL when the frame is not IPv4, or is too short
    size_t                ip_avail;  // bytes readable from ip onwards
} wan_frame_t;

static bool frame_window(struct pbuf *p, uint8_t *scratch, wan_frame_t *out)
{
    const size_t eth_ip_len = sizeof(struct eth_hdr) + IP_HLEN;
    size_t have_len;

    memset(out, 0, sizeof(*out));
    if (p == NULL) {
        return false;
    }

    if (p->len >= eth_ip_len) {
        out->eth = (const struct eth_hdr *)p->payload;
        have_len = p->len;
    } else if (p->tot_len >= eth_ip_len) {
        have_len = (p->tot_len < WAN_HDR_WINDOW) ? p->tot_len : WAN_HDR_WINDOW;
        if (pbuf_copy_partial(p, scratch, have_len, 0) != have_len) {
            return false;
        }
        out->eth = (const struct eth_hdr *)scratch;
    } else if (p->len >= sizeof(struct eth_hdr)) {
        // Too short for IP but the ethertype is readable - ARP lands here.
        out->eth = (const struct eth_hdr *)p->payload;
        return true;
    } else if (p->tot_len >= sizeof(struct eth_hdr)) {
        if (pbuf_copy_partial(p, scratch, sizeof(struct eth_hdr), 0) != sizeof(struct eth_hdr)) {
            return false;
        }
        out->eth = (const struct eth_hdr *)scratch;
        return true;
    } else {
        return false;
    }

    if (out->eth->type == PP_HTONS(ETHTYPE_IP)) {
        out->ip = (const struct ip_hdr *)((const uint8_t *)out->eth + sizeof(struct eth_hdr));
        out->ip_avail = have_len - sizeof(struct eth_hdr);
    }
    return true;
}

// Destination port of a TCP or UDP header sitting immediately after the IP
// header, in host order. Returns false when the window does not reach it.
static bool l4_ports(const wan_frame_t *f, uint16_t *sport, uint16_t *dport)
{
    if (f->ip == NULL || IPH_HL(f->ip) != 5 || f->ip_avail < IP_HLEN + 4) {
        return false;
    }
    const uint8_t *l4 = (const uint8_t *)f->ip + IP_HLEN;
    *sport = (uint16_t)((l4[0] << 8) | l4[1]);
    *dport = (uint16_t)((l4[2] << 8) | l4[3]);
    return true;
}

static uint8_t icmp_type(const wan_frame_t *f)
{
    if (f->ip == NULL || IPH_HL(f->ip) != 5 || f->ip_avail < IP_HLEN + 1) {
        return 0xff;
    }
    return *((const uint8_t *)f->ip + IP_HLEN);
}

// Egress: LAN -> internet, plus anything this device sends of its own accord.
//
// This runs AFTER translation - ip4_forward() calls ip_napt_forward() before
// netif->output() - so a source address here is always the STA's own and cannot
// be used to tell a forwarded packet from a locally generated one. That is fine:
// every rule below is about the destination, which translation leaves alone. The
// consequence to know about is that a packet this returns false for has already
// taken a NAPT table entry, which is why IP_NAPT_MAX is sized with headroom in
// the root CMakeLists.txt and why tx_blocked is surfaced in the UI.
static bool egress_allowed(const wan_frame_t *f, const wan_rules_t *r)
{
    // ARP first and unconditionally. Without it the station never resolves its
    // own gateway and nothing else can possibly work.
    if (f->eth->type == PP_HTONS(ETHTYPE_ARP)) {
        return true;
    }
    // CONFIG_LWIP_IPV6 is on, so this is a live case rather than a formality:
    // anything that is not IPv4 has no NAT and no policy, and leaves by no door.
    if (f->eth->type != PP_HTONS(ETHTYPE_IP) || f->ip == NULL) {
        return false;
    }
    // Nothing in this stack emits IP options, and they would move the L4 header
    // off the fixed offset the rules read. Refusing beats mis-parsing - the same
    // call is_dhcp_packet() makes.
    if (IPH_HL(f->ip) != 5) {
        return false;
    }

    uint16_t sport, dport;

    // While the uplink is down there is nowhere for anything to go, but the
    // station still has to be able to get itself an address.
    if (IPH_PROTO(f->ip) == IP_PROTO_UDP && l4_ports(f, &sport, &dport) &&
        sport == 68 && dport == 67) {
        return true;
    }
    if (!r->up) {
        return false;
    }

    switch (IPH_PROTO(f->ip)) {
    case IP_PROTO_ICMP:
        // Outbound ping only. It is the one diagnostic an operator has for "is
        // the uplink actually carrying anything", and it reveals nothing.
        return icmp_type(f) == ICMP_ECHO;

    case IP_PROTO_UDP:
    case IP_PROTO_TCP:
        if (!l4_ports(f, &sport, &dport)) {
            return false;
        }
        // DNS to the resolver we were handed, and to no other. Allowing 53 to
        // anywhere would let a client pick its own resolver and, with it, its
        // own answers - and the whole point of the list is that what leaves this
        // device is enumerable.
        if (dport == 53 && r->dns_ip != 0 && f->ip->dest.addr == r->dns_ip) {
            return true;
        }
        // Protocol as well as number. The list started TCP-only, on the
        // reasoning that a UDP port is a hole with no session behind it and
        // NTRIP is TCP; RustDesk is what changed it, since its ID registration
        // and heartbeat are UDP and it does not work without them.
        {
            uint8_t want = (IPH_PROTO(f->ip) == IP_PROTO_UDP) ? WAN_PROTO_UDP : WAN_PROTO_TCP;
            for (uint8_t i = 0; i < r->nports; i++) {
                if (r->ports[i].port == dport && r->ports[i].proto == want) {
                    return true;
                }
            }
        }
        return false;

    default:
        return false;
    }
}

// Ingress: internet -> this device. NAPT does NOT stop unsolicited inbound -
// ip_napt_recv() leaves the destination untouched when there is no mapping, and
// ip4_input_accept() then accepts the packet for the STA's own address. The web
// server binds INADDR_ANY (esp_http_server has no bind-address option), so
// without this filter the admin UI, and /api/ota with it, would be reachable
// from the upstream network the moment the station gets an address.
//
// The rule that closes it is stateless and exact: ip_napt_new_port() can only
// ever return a port inside [IP_NAPT_PORT_RANGE_START, IP_NAPT_PORT_RANGE_END],
// so every NAT return packet lands on a destination port in that window and
// nothing else legitimately does.
static bool ingress_allowed(const wan_frame_t *f, const wan_rules_t *r)
{
    (void)r;

    if (f->eth->type == PP_HTONS(ETHTYPE_ARP)) {
        return true;
    }
    if (f->eth->type != PP_HTONS(ETHTYPE_IP) || f->ip == NULL) {
        return false;
    }
    if (IPH_HL(f->ip) != 5) {
        return false;
    }

    uint16_t sport, dport;

    switch (IPH_PROTO(f->ip)) {
    case IP_PROTO_ICMP: {
        // Replies and errors, but deliberately not ICMP_ECHO: the WAN side does
        // not get to ping this device.
        uint8_t t = icmp_type(f);
        return t == ICMP_ER || t == ICMP_DUR || t == ICMP_TE;
    }

    case IP_PROTO_UDP:
    case IP_PROTO_TCP:
        if (!l4_ports(f, &sport, &dport)) {
            return false;
        }
        if (dport == 68) {
            return true;  // our own DHCP client's offer/ack
        }
        return dport >= IP_NAPT_PORT_RANGE_START && dport <= IP_NAPT_PORT_RANGE_END;

    default:
        return false;
    }
}

static err_t sta_input_wrapper(struct pbuf *p, struct netif *inp)
{
    uint8_t scratch[WAN_HDR_WINDOW];
    wan_frame_t f;
    const wan_rules_t *r = s_active;

    if (frame_window(p, scratch, &f) && !ingress_allowed(&f, r)) {
        s_status.rx_blocked++;
        // Both drivers free the pbuf themselves when input returns anything but
        // ERR_OK, so this frees and reports success instead, keeping them on the
        // path they take for a frame that was delivered - see the same note in
        // traffic_input_wrapper().
        pbuf_free(p);
        return ERR_OK;
    }
    s_status.rx_allowed++;
    return s_sta_orig_input(p, inp);
}

static err_t sta_output_wrapper(struct netif *netif, struct pbuf *p)
{
    uint8_t scratch[WAN_HDR_WINDOW];
    wan_frame_t f;
    const wan_rules_t *r = s_active;

    if (frame_window(p, scratch, &f) && !egress_allowed(&f, r)) {
        s_status.tx_blocked++;
        // No pbuf_free() here, unlike the RX side: linkoutput never owns what it
        // is handed.
        return ERR_OK;
    }
    s_status.tx_allowed++;
    return s_sta_orig_linkoutput(netif, p);
}

static void on_sta_port_started(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (s_sta_orig_input != NULL) {
        return;  // already hooked
    }
    struct netif *nif = (struct netif *)esp_netif_get_netif_impl(s_sta_netif);
    if (nif == NULL) {
        ESP_LOGW(TAG, "sta netif impl not ready yet");
        return;
    }
    // Unlike the bridge ports, this input is tcpip_input rather than
    // bridgeif_input, so the ingress wrapper runs in the WiFi driver task before
    // the tcpip message is posted - the cheapest point at which a packet from
    // the internet can be refused.
    s_sta_orig_input = nif->input;
    s_sta_orig_linkoutput = nif->linkoutput;
    nif->input = sta_input_wrapper;
    nif->linkoutput = sta_output_wrapper;
    ESP_LOGI(TAG, "WAN packet filter installed on the uplink");

    s_status.state = WAN_STATE_CONNECTING;
    esp_wifi_connect();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void retry_timer_cb(void *arg)
{
    s_status.retry_in_s = 0;
    esp_wifi_connect();
}

static void schedule_retry(uint32_t seconds)
{
    if (s_retry_timer == NULL) {
        return;
    }
    esp_timer_stop(s_retry_timer);  // harmless if not armed
    s_status.retry_in_s = seconds;
    esp_timer_start_once(s_retry_timer, (uint64_t)seconds * 1000000ULL);
}

// Enabling NAPT is idempotent and permanent. Never disabled on an uplink flap:
// ip_napt_enable_netif(netif, 0) calls ip_napt_deinit(), which frees the table,
// and ip_napt_init() guards its allocation with assert() - so a flapping uplink
// would mean freeing and re-allocating 8 KB on a heap at its most fragmented,
// behind an abort. With the station down there is no route out anyway, so an
// enabled-but-idle NAPT costs nothing beyond the table.
//
// The bridge is the *inside* netif: ip4.c skips translation when the output
// netif carries the flag and skips the reverse lookup when the input netif does,
// so the flag marks the inside, not the uplink.
static void napt_ensure(void)
{
    if (s_napt_on || s_br_netif == NULL) {
        return;
    }
    esp_err_t err = esp_netif_napt_enable(s_br_netif);
    if (err == ESP_OK) {
        s_napt_on = true;
        s_status.napt_on = true;
        ESP_LOGI(TAG, "NAPT enabled on the bridge");
    } else {
        // Expected when the bridge netif is not up yet; the IP_EVENT_NETIF_UP
        // handler tries again.
        ESP_LOGD(TAG, "NAPT not enabled yet (%s)", esp_err_to_name(err));
    }
}

// IP_EVENT_NETIF_UP rather than a got-IP event, for the reason
// br_netif_status_cb() in dhcp_server.c records: the bridge netif comes from
// ESP_NETIF_INHERENT_DEFAULT_BR_DHCPS(), whose get_ip_event is 0, so nothing
// raises IP_EVENT_*_GOT_IP for it. NETIF_UP is posted for every netif, hence the
// filter.
static void on_bridge_up(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_netif_status_t *evt = data;
    if (evt == NULL || evt->esp_netif != s_br_netif) {
        return;
    }
    napt_ensure();
}

static void on_sta_connected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    wifi_event_sta_connected_t *evt = (wifi_event_sta_connected_t *)data;

    s_status.connects++;
    s_auth_fails = 0;

    uint8_t primary = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
        s_status.ap_channel = primary;
    }

    char ssid[WIFI_CFG_SSID_MAX_LEN], password[WIFI_CFG_PASSWORD_MAX_LEN];
    uint8_t configured = 0;
    wifi_cfg_load(ssid, password, &configured);
    s_status.ap_channel_configured = configured;

    ESP_LOGI(TAG, "uplink associated to %.*s on channel %u",
             evt ? evt->ssid_len : 0, evt ? (const char *)evt->ssid : "", primary);

    // The single-radio consequence, said out loud rather than left for somebody
    // to discover from a field full of clients that vanished.
    if (configured != 0 && primary != 0 && primary != configured) {
        ESP_LOGW(TAG, "this bridge's WiFi has moved from channel %u to channel %u to match "
                      "the upstream network - every associated client has just re-associated",
                 configured, primary);
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    if (evt == NULL) {
        return;
    }

    // Two networks overlap when they agree on every bit that both masks cover.
    // Tested this way rather than "is the address inside 192.168.5.0/24":
    // an upstream handing out a /16 at 192.168.0.0 collides just as fatally and
    // would sail through the narrower test.
    uint32_t common = evt->ip_info.netmask.addr & LAN_MASK;
    if (((evt->ip_info.ip.addr ^ LAN_IP) & common) == 0) {
        ESP_LOGE(TAG, "upstream network gave us " IPSTR "/" IPSTR ", which overlaps this "
                      "bridge's own 192.168.5.0/24 - the uplink cannot be used. Change the "
                      "upstream router's address range.",
                 IP2STR(&evt->ip_info.ip), IP2STR(&evt->ip_info.netmask));
        s_status.state = WAN_STATE_SUBNET_CONFLICT;
        s_status.ip.addr = 0;
        s_status.dns.addr = 0;
        publish_rules(false, 0);
        // Not silently degraded: a half-working uplink whose LAN routing has
        // quietly become ambiguous is worse than no uplink at all.
        esp_wifi_disconnect();
        schedule_retry(BACKOFF_REJECTED_S);
        return;
    }

    s_status.ip = evt->ip_info.ip;
    s_status.netmask = evt->ip_info.netmask;
    s_status.gw = evt->ip_info.gw;

    esp_netif_dns_info_t dns;
    uint32_t dns_ip = 0;
    if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        dns_ip = dns.ip.u_addr.ip4.addr;
    }
    s_status.dns.addr = dns_ip;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_status.rssi = ap.rssi;
    }

    s_status.state = WAN_STATE_UP;
    s_backoff_s = BACKOFF_MIN_S;
    s_status.retry_in_s = 0;

    publish_rules(true, dns_ip);
    napt_ensure();

    ESP_LOGI(TAG, "uplink up: " IPSTR " via " IPSTR ", DNS " IPSTR,
             IP2STR(&evt->ip_info.ip), IP2STR(&evt->ip_info.gw), IP2STR(&s_status.dns));
    // The route-priority election, not an override, is what moves the default
    // route here - the STA netif is priority 100 against the bridge's 70. Logged
    // so that which interface won is visible rather than inferred.
    esp_netif_t *def = esp_netif_get_default_netif();
    ESP_LOGI(TAG, "default interface is now %s", def ? esp_netif_get_desc(def) : "(none)");
}

static void on_lost_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (s_status.state == WAN_STATE_UP) {
        s_status.state = WAN_STATE_DOWN;
    }
    s_status.ip.addr = 0;
    s_status.dns.addr = 0;
    publish_rules(false, 0);
    ESP_LOGW(TAG, "uplink lost its address");
}

static void on_sta_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)data;
    uint8_t reason = evt ? evt->reason : 0;

    s_status.disconnects++;
    s_status.last_reason = reason;
    s_status.ip.addr = 0;
    s_status.dns.addr = 0;
    publish_rules(false, 0);

    // A conflict has already scheduled its own long retry and said why; letting
    // the ordinary backoff below overwrite that would turn a clear diagnosis
    // into a fast reconnect loop that reproduces it every few seconds.
    if (s_status.state == WAN_STATE_SUBNET_CONFLICT) {
        return;
    }

    uint32_t delay;
    if (reason == WIFI_REASON_AUTH_FAIL ||
        reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
        reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
        if (++s_auth_fails >= AUTH_FAILS_BEFORE_SLOW) {
            s_status.state = WAN_STATE_AUTH_FAILED;
            delay = BACKOFF_REJECTED_S;
            ESP_LOGE(TAG, "upstream network refused our password (reason %u) - retrying every "
                          "%u s in case it is corrected on the other end", reason, (unsigned)delay);
        } else {
            s_status.state = WAN_STATE_CONNECTING;
            delay = s_backoff_s;
        }
    } else {
        if (reason == WIFI_REASON_NO_AP_FOUND) {
            s_status.state = WAN_STATE_NO_AP;
        } else if (s_status.state != WAN_STATE_CONNECTING) {
            s_status.state = WAN_STATE_DOWN;
        }
        delay = s_backoff_s;
    }

    if (delay == s_backoff_s) {
        s_backoff_s *= 2;
        if (s_backoff_s > BACKOFF_MAX_S) {
            s_backoff_s = BACKOFF_MAX_S;
        }
    }

    ESP_LOGW(TAG, "uplink disconnected (reason %u), retrying in %u s", reason, (unsigned)delay);
    schedule_retry(delay);
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

void wan_get_status(wan_status_t *out)
{
    *out = s_status;
}

uint32_t wan_get_dns(void)
{
    const wan_rules_t *r = s_active;
    return r->up ? r->dns_ip : 0;
}

bool wan_is_up(void)
{
    return s_status.state == WAN_STATE_UP;
}

void wan_ports_changed(void)
{
    const wan_rules_t *r = s_active;
    publish_rules(r->up, r->dns_ip);
}

const char *wan_state_name(wan_state_t s)
{
    switch (s) {
    case WAN_STATE_DISABLED:        return "disabled";
    case WAN_STATE_CONNECTING:      return "connecting";
    case WAN_STATE_UP:              return "up";
    case WAN_STATE_DOWN:            return "down";
    case WAN_STATE_NO_AP:           return "no_ap";
    case WAN_STATE_AUTH_FAILED:     return "auth_failed";
    case WAN_STATE_SUBNET_CONFLICT: return "subnet_conflict";
    }
    return "unknown";
}

esp_err_t wan_init(esp_netif_t *br_netif)
{
    s_br_netif = br_netif;

    wan_cfg_t cfg;
    wan_cfg_get(&cfg);

    // The whole "identical to today" guarantee lives here: with no uplink
    // configured this creates no netif, leaves the radio in WIFI_MODE_AP,
    // enables no NAPT, installs no hooks and allocates nothing. wan_get_dns()
    // then returns 0 and the DHCP server hands out 192.168.5.1 exactly as it
    // always did.
    if (!cfg.enabled) {
        s_status.state = WAN_STATE_DISABLED;
        ESP_LOGI(TAG, "internet uplink not configured - bridge only");
        return ESP_OK;
    }

    s_publish_mutex = xSemaphoreCreateMutex();
    if (s_publish_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    publish_rules(false, 0);

    esp_netif_inherent_config_t sta_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    s_sta_netif = esp_netif_create_wifi(WIFI_IF_STA, &sta_cfg);
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "could not create the uplink interface");
        return ESP_ERR_NO_MEM;
    }

    // esp_wifi_set_default_wifi_ap_handlers(), already called in
    // wifi_init_softap(), registered IDF's STA handlers too - they were inert
    // only because no STA netif existed. esp_netif_create_wifi() attaches this
    // one, so there is nothing more to register, and esp_event's in-order
    // dispatch then guarantees the handlers below run after IDF's own.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strlcpy((char *)wc.sta.ssid, cfg.ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, cfg.password, sizeof(wc.sta.password));
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.threshold.authmode = cfg.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    // PMF left at its defaults. wifi_cfg_apply()'s
    // esp_wifi_disable_pmf_config(WIFI_IF_AP) is about stations associated to
    // *our* AP and the airtime SA Query costs them; as a client of somebody
    // else's AP none of that reasoning applies.
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    const esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .name = "wan_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START,
                                               &on_sta_port_started, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                               &on_sta_connected, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                               &on_sta_disconnected, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                               &on_lost_ip, NULL));
    // esp_netif_napt_enable() fails while its target netif is down, and the
    // bridge comes up on the first link-up or association - after this runs. The
    // same filtered-by-netif trick dhcp_server.c uses to find the bridge.
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_NETIF_UP,
                                               &on_bridge_up, NULL));

    s_status.state = WAN_STATE_CONNECTING;
    ESP_LOGI(TAG, "internet uplink configured for \"%s\"", cfg.ssid);
    return ESP_OK;
}
