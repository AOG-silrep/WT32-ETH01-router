#include <string.h>
#include "client_track.h"
#include "esp_netif_net_stack.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"
#include "netif/ethernet.h"

static const char *TAG = "client_track";

#define CLIENT_AGE_OUT_US (5LL * 60 * 1000000)   // drop a client after 5 min of silence
#define TICK_PERIOD_MS    1000

typedef struct {
    bool active;
    uint8_t mac[6];
    esp_ip4_addr_t ip;
    bool is_wifi;
    int8_t rssi;
    uint32_t rx_bytes, tx_bytes;           // cumulative
    uint32_t rx_bytes_prev, tx_bytes_prev; // previous tick's cumulative, for rate calc
    uint32_t rx_bps, tx_bps;
    int64_t last_seen_us;
    char name[CLIENT_TRACK_NAME_MAX_LEN];
} client_entry_t;

static client_entry_t s_clients[CLIENT_TRACK_MAX_CLIENTS];
static SemaphoreHandle_t s_mutex;

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
static void update_client_ip_locked(client_entry_t *e, esp_ip4_addr_t new_ip)
{
    if (new_ip.addr == 0) {
        return;
    }
    if (e->ip.addr != 0 && e->ip.addr != new_ip.addr) {
        char old_str[16], new_str[16];
        esp_ip4addr_ntoa(&e->ip, old_str, sizeof(old_str));
        esp_ip4addr_ntoa(&new_ip, new_str, sizeof(new_str));
        ESP_LOGW(TAG, "client %02x:%02x:%02x:%02x:%02x:%02x changed IP: %s -> %s",
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                 old_str, new_str);
    }
    e->ip = new_ip;
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
        } else {
            e->rx_bytes += bytes;
        }
        e->last_seen_us = esp_timer_get_time();
        if (observed_ip != NULL) {
            update_client_ip_locked(e, *observed_ip);
        }
    }
    xSemaphoreGive(s_mutex);
}

// RX wrapper (client -> bridge, uplink). Installed in place of the input
// pointer the bridge glue already assigned, so we chain to that, not to
// the raw driver input.
static err_t traffic_input_wrapper(struct pbuf *p, struct netif *inp, bool is_wifi,
                                    netif_input_fn orig)
{
    if (p != NULL && p->len >= 12) {
        struct eth_hdr *hdr = (struct eth_hdr *)p->payload;
        if (mac_is_client(hdr->src.addr)) {
            // Sniff the source IP straight off the client's own packet when
            // possible - some clients re-address themselves after DHCP
            // without ever renewing the lease, so the DHCP lease table
            // client_track_tick() polls can go stale while this can't.
            esp_ip4_addr_t src_ip;
            const esp_ip4_addr_t *src_ip_p = NULL;
            if (hdr->type == PP_HTONS(ETHTYPE_IP) &&
                p->len >= sizeof(struct eth_hdr) + IP_HLEN) {
                struct ip_hdr *iph = (struct ip_hdr *)((uint8_t *)p->payload + sizeof(struct eth_hdr));
                src_ip.addr = iph->src.addr;
                src_ip_p = &src_ip;
            }
            account_traffic(hdr->src.addr, is_wifi, p->tot_len, true, src_ip_p);
        }
    }
    return orig(p, inp);
}

static err_t eth_input_wrapper(struct pbuf *p, struct netif *inp)
{
    return traffic_input_wrapper(p, inp, false, s_eth_orig_input);
}

static err_t wifi_input_wrapper(struct pbuf *p, struct netif *inp)
{
    return traffic_input_wrapper(p, inp, true, s_wifi_orig_input);
}

// TX wrapper (bridge -> client, downlink). Broadcast/multicast frames are
// deliberately not attributed to any single client.
static err_t traffic_output_wrapper(struct netif *netif, struct pbuf *p, bool is_wifi,
                                     netif_linkoutput_fn orig)
{
    if (p != NULL && p->len >= 12) {
        struct eth_hdr *hdr = (struct eth_hdr *)p->payload;
        if (mac_is_client(hdr->dest.addr)) {
            account_traffic(hdr->dest.addr, is_wifi, p->tot_len, false, NULL);
        }
    }
    return orig(netif, p);
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
            update_client_ip_locked(e, evt->ip);
        }
        if (evt->hostname[0] != '\0') {
            strlcpy(e->name, evt->hostname, sizeof(e->name));
        }
    }
    xSemaphoreGive(s_mutex);
}

static void client_track_tick(void)
{
    int64_t now = esp_timer_get_time();
    uint8_t macs[CLIENT_TRACK_MAX_CLIENTS][6];
    int n = 0;

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
        memcpy(macs[n], e->mac, 6);
        n++;
    }
    xSemaphoreGive(s_mutex);

    // Resolve IPs outside the lock: DHCP lease table first, ARP cache as a
    // fallback for statically-addressed clients. This is only a bootstrap
    // for clients we haven't directly observed traffic from yet - once we
    // have a real IP (via on_client_ip_assigned or sniffed uplink traffic),
    // it takes priority, since the lease table can go stale for clients
    // that re-address themselves without renewing.
    esp_netif_pair_mac_ip_t pairs[CLIENT_TRACK_MAX_CLIENTS];
    for (int i = 0; i < n; i++) {
        memcpy(pairs[i].mac, macs[i], 6);
        pairs[i].ip.addr = 0;
    }
    if (n > 0) {
        esp_netif_dhcps_get_clients_by_mac(s_br_netif, n, pairs);
        for (int i = 0; i < n; i++) {
            if (pairs[i].ip.addr == 0) {
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
            update_client_ip_locked(e, pairs[i].ip);
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
}

static void client_track_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TICK_PERIOD_MS));
        client_track_tick();
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

    BaseType_t ok = xTaskCreatePinnedToCore(client_track_task, "client_track", 4096,
                                             NULL, tskIDLE_PRIORITY + 2, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
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
        int64_t age_us = now - e->last_seen_us;
        o->last_seen_s = age_us > 0 ? (uint32_t)(age_us / 1000000) : 0;
        strlcpy(o->name, e->name, sizeof(o->name));
    }
    xSemaphoreGive(s_mutex);

    *count = n;
}
