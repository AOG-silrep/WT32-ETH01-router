// DHCP server for the bridge LAN, with MAC -> IP reservations kept in flash.
//
// This exists instead of ESP-IDF's DHCP server, not alongside it. IDF's server
// keeps its lease pool in RAM and rebuilds it empty on every start, then hands
// out addresses sequentially from a rolling counter; a returning client that
// asks for the address it held before its last boot is NAK'd outright, because
// the server compares the requested address against the one it just allocated
// and rejects any mismatch. Reboot the bridge, bring the clients up in a
// different order, and every address moves.
//
// There is no way to fix that from outside. ESP-IDF exposes no reservation API
// (esp_netif.h has dhcps_start/stop/option/get_status and a read-only
// get_clients_by_mac), dhcps_t is opaque so its pool cannot be pre-seeded, and
// LWIP_HOOK_DHCPS_POST_STATE runs after the address has already been chosen.
// Note also that CONFIG_LWIP_DHCPS_STATIC_ENTRIES, which is enabled in
// sdkconfig, is not a reservation feature despite the name - it maps to
// ETHARP_SUPPORT_STATIC_ENTRIES and only adds an ARP entry for an address that
// was already leased.
//
// So the server here owns port 67. The one thing it does differently is
// consult the stored reservation for a MAC *before* allocating anything, which
// is what makes join order stop mattering across a reboot.

#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "dhcp_server.h"
#include "client_track.h"   // client_track_mac_on_wired_port()
#include "wan.h"            // wan_get_dns()

static const char *TAG = "dhcp_server";

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

// 7200s, matching what ESP-IDF's server offered (DHCPS_LEASE_TIME_DEF 120 *
// CONFIG_LWIP_DHCPS_LEASE_UNIT 60), so clients see no change in renew timing.
#define LEASE_TIME_S 7200

// How long an address is held between the OFFER and the REQUEST that confirms
// it, so two clients discovering at the same time can't be offered the same
// address. Short, because a client that never comes back for it shouldn't tie
// an address up.
#define OFFER_HOLD_S 30

// Addresses a client refused with DHCPDECLINE (its ARP probe found the address
// already in use). Kept only for this boot - a conflict is a property of the
// network right now, not something worth remembering in flash.
#define MAX_DECLINED 8

// Wait this long after a mapping changes before writing it to flash, so a
// burst of clients joining at once costs one write rather than one each.
#define SAVE_DEBOUNCE_US (10 * 1000 * 1000)

// Forget a reservation whose client has not been heard from for this many boot
// generations. Counted in generations rather than hours because hours are not
// measurable here (see s_boot_seq), and applied at load because that is the only
// moment the count changes.
//
// Five is the point at which a mapping has stopped describing the network. A
// laptop that is away for a weekend, or a tractor parked for the winter, comes
// back well inside it and gets its address; a device that has missed five
// separate boots is one that has been unplugged for good, and keeping its
// address out of circulation forever helps nobody. The address was already
// reclaimable on demand before this - what this adds is that the table stops
// carrying entries nobody will ask about again, so /leases reads as a picture of
// the network rather than of its history.
#define FORGET_AFTER_BOOTS 5

// How rarely to repeat the warning that the wired range is full and a wired
// client is being served from the WiFi one. See note_wired_range_full().
#define WIRED_FULL_LOG_US (5LL * 60 * 1000000)

// BOOTP/DHCP message. Packed and byte-order-explicit because it goes on the
// wire exactly as laid out here.
typedef struct __attribute__((packed)) {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint8_t  options[312];
} dhcp_msg_t;

#define DHCP_FIXED_LEN   236   // everything above options
#define DHCP_MIN_REPLY   300   // BOOTP minimum; pad short replies up to it

#define BOOTREQUEST 1
#define BOOTREPLY   2

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPDECLINE  4
#define DHCPACK      5
#define DHCPNAK      6
#define DHCPRELEASE  7
#define DHCPINFORM   8

#define OPT_SUBNET_MASK   1
#define OPT_ROUTER        3
#define OPT_DNS_SERVER    6
#define OPT_HOSTNAME     12
#define OPT_INTERFACE_MTU 26
#define OPT_BROADCAST     28
#define OPT_ROUTER_DISC   31
#define OPT_REQUESTED_IP  50
#define OPT_LEASE_TIME    51
#define OPT_MSG_TYPE      53
#define OPT_SERVER_ID     54
#define OPT_END         255

// ---- lease table ----

typedef struct {
    bool     in_use;
    uint8_t  mac[6];
    uint32_t ip;            // address this server leased, network byte order
    // Address the client was actually seen using, when that is not the one
    // leased to it - i.e. one it assigned itself. 0 when the two agree, or
    // when nothing has been observed. Kept apart from ip rather than
    // overwriting it: the lease is this server's allocation bookkeeping, and
    // losing track of it would let the address go to somebody else while the
    // client is still renewing it in the background.
    uint32_t observed_ip;
    uint32_t saved_ip;          // ip as last written to flash, 0 if never
    uint32_t saved_observed_ip; // observed_ip as last written to flash
    int64_t  expires_us;    // 0 = reservation restored from flash, never claimed
    // Which boot generation this client was last heard from, and when within
    // that boot. Together they order entries oldest-first for reclaiming.
    // last_seen_us alone cannot: it restarts at 0 every boot, so after a
    // restart every restored entry ties and "least recently seen" collapses
    // into array order. See s_boot_seq.
    uint32_t last_seen_seq;
    int64_t  last_seen_us;
    // This client is cut off for sitting on an address that was already
    // somebody else's - see client_track.c, which decides this and pushes the
    // set in through dhcp_server_set_quarantined(). Never persisted: it is a
    // judgement about the network as it is now, and the next boot re-derives it
    // from what it observes rather than inheriting it.
    bool     quarantined;
} lease_t;

static lease_t s_leases[DHCP_SERVER_MAX_LEASES];
static uint32_t s_declined[MAX_DECLINED];
static int s_declined_count;
static SemaphoreHandle_t s_mutex;

static esp_netif_t *s_br_netif;
static uint32_t s_server_ip, s_netmask, s_pool_start, s_pool_end;
// The range served to clients that ask over the cable, or 0/0 if the bridge is
// serving one range to everybody. Kept clear of [s_pool_start, s_pool_end], so
// which range a client is served from is decided in one place - the two lines at
// the top of select_address_locked() - rather than by a check that has to be
// remembered at each of the four steps below it.
//
// The point of a range rather than a single reserved address is that the wired
// port is not one device. A laptop with two Ethernet interfaces swapped on the
// same cable is two MACs, and a switch on that port is as many as somebody plugs
// into it; each of them wants an address of its own, and wants the same one back
// next time.
static uint32_t s_wired_start, s_wired_end;
static int s_sock = -1;
static int s_restored_count;

// This boot's generation number, one past whatever was last written to flash.
//
// Reservations are never expired by elapsed time, because this device cannot
// measure it: esp_timer_get_time() restarts at 0 every boot, RTC memory is
// wiped by power-on, there is no RTC battery, and a transparent L2 bridge has
// no uplink it can count on for SNTP - a board unplugged for a month is
// indistinguishable from one power-cycled a second ago. Reclaiming under
// pressure needs only *ordering*, though, and that much is persistable: an
// entry stamped with an older generation than another was seen longer ago,
// whatever the wall-clock gap between them was.
//
// It advances only on a boot that writes the table, which is exactly a boot
// that changed something - so this costs no extra flash writes. Two quiet
// boots sharing a generation is harmless, neither changed anything.
static uint32_t s_boot_seq;

// Set when the table's MAC -> IP set differs from what is in flash. Compared
// against each entry's saved_ip rather than against whether the entry changed,
// so that the address written at OFFER time and confirmed unchanged at ACK
// time still counts as new. Renewals, which are the overwhelming majority of
// transactions, never set it.
static bool s_dirty;
static int64_t s_dirty_since_us;

static void mark_dirty_locked(void)
{
    if (!s_dirty) {
        s_dirty = true;
        s_dirty_since_us = esp_timer_get_time();
    }
}

// What this entry's observed address should look like in flash, as opposed to
// what it looks like in RAM. The two differ for exactly one reason: a client
// cut off for holding a duplicate address contributes nothing.
//
// A restart is the one thing that clears a block, so nothing about one may
// outlive it - and persisting the disputed address would do worse than outlive
// it, it would invert it. The block itself is RAM-only and does go, so on the
// next boot the entry comes back unquarantined with its claim on the contested
// address intact and fully counted, while the device that *kept* the address
// comes back with an expired lease that claims nothing. ip_in_use_locked()
// would then refuse to give the winner its own address back and renumber it in
// favour of the device that had been cut off.
//
// So the disputed address is kept out of flash entirely. If that leaves nothing
// worth storing, the entry is dropped from the blob; either way the next boot
// starts with no memory of the argument and re-derives it from live traffic.
static uint32_t persisted_observed_ip(const lease_t *e)
{
    return e->quarantined ? 0 : e->observed_ip;
}

// ---- NVS ----

#define NVS_NAMESPACE "dhcp_leases"
#define NVS_TABLE_KEY "table"
#define STORE_MAGIC   0x4C504344  // 'DCPL'
// 1 had no observed_ip, 2 no last_seen_seq. Both still readable - see
// load_from_nvs(), which keeps a device's learnt reservations across an
// upgrade rather than starting it over with an empty table.
#define STORE_VERSION 3

// Only the MAC and the addresses are persisted: 14 bytes an entry, so the
// whole table is well under a kilobyte of a 24KB NVS partition (see
// partitions.csv), and a rewrite costs little before garbage collection.
// Lease expiry means nothing across a reboot, and hostnames are re-learned
// from option 12 on the client's next transaction - which is already how they
// reach the UI.
typedef struct __attribute__((packed)) {
    uint8_t  mac[6];
    uint32_t ip;
    uint32_t observed_ip;
    uint32_t last_seen_seq;
} stored_lease_t;

// Earlier revisions of the same record, read so that upgrading keeps the
// reservations a device already learnt. Each is a strict prefix of the one
// after it, which is what lets load_from_nvs() copy rec_size bytes into a
// zeroed stored_lease_t and have the missing fields land as 0.
typedef struct __attribute__((packed)) {
    uint8_t  mac[6];
    uint32_t ip;
} stored_lease_v1_t;

typedef struct __attribute__((packed)) {
    uint8_t  mac[6];
    uint32_t ip;
    uint32_t observed_ip;
} stored_lease_v2_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint32_t boot_seq;   // absent before version 3; reads as 0
} stored_hdr_t;

// The header grew in version 3, so a v1/v2 blob has a shorter one.
#define STORED_HDR_LEN(ver) \
    ((ver) >= 3 ? sizeof(stored_hdr_t) : offsetof(stored_hdr_t, boot_seq))

static void load_from_nvs(void)
{
    // Generation 1 unless flash says otherwise, so "seen this boot" is a
    // meaningful test even with nothing restored.
    s_boot_seq = 1;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no saved leases (%s) - starting with an empty table",
                 esp_err_to_name(err));
        return;
    }

    uint8_t buf[sizeof(stored_hdr_t) + DHCP_SERVER_MAX_LEASES * sizeof(stored_lease_t)];
    size_t len = sizeof(buf);
    err = nvs_get_blob(h, NVS_TABLE_KEY, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no saved leases (%s) - starting with an empty table",
                 esp_err_to_name(err));
        return;
    }

    // Only the pre-v3 prefix is needed to read the version out; the full
    // header is only present from v3 on.
    if (len < STORED_HDR_LEN(1)) {
        ESP_LOGW(TAG, "saved leases truncated (%u bytes) - ignoring", (unsigned)len);
        return;
    }
    stored_hdr_t hdr = { 0 };
    memcpy(&hdr, buf, STORED_HDR_LEN(1));
    if (hdr.magic != STORE_MAGIC || hdr.version < 1 || hdr.version > STORE_VERSION) {
        ESP_LOGW(TAG, "saved leases are not a table this build understands "
                      "(magic %08x, version %u) - ignoring",
                 (unsigned)hdr.magic, (unsigned)hdr.version);
        return;
    }

    size_t hdr_len = STORED_HDR_LEN(hdr.version);
    if (len < hdr_len) {
        ESP_LOGW(TAG, "saved leases truncated (%u bytes) - ignoring", (unsigned)len);
        return;
    }
    memcpy(&hdr, buf, hdr_len);   // picks up boot_seq on v3

    size_t rec_size = (hdr.version == 1) ? sizeof(stored_lease_v1_t)
                    : (hdr.version == 2) ? sizeof(stored_lease_v2_t)
                                         : sizeof(stored_lease_t);
    if (hdr.count > DHCP_SERVER_MAX_LEASES ||
        len < hdr_len + (size_t)hdr.count * rec_size) {
        ESP_LOGW(TAG, "saved leases claim %u entries in %u bytes - ignoring",
                 (unsigned)hdr.count, (unsigned)len);
        return;
    }

    // One past what was last written, so everything restored below sorts as
    // older than anything this boot goes on to hear from.
    s_boot_seq = hdr.boot_seq + 1;

    int n = 0;
    int forgotten = 0;
    for (int i = 0; i < hdr.count; i++) {
        // Zeroed, so fields the stored version predates land as 0: no observed
        // address on v1, and generation 0 - maximally stale - on v1 and v2.
        // That self-corrects the moment each client is heard from again.
        stored_lease_t s = { 0 };
        memcpy(&s, buf + hdr_len + i * rec_size, rec_size);

        // Drop a client that has missed FORGET_AFTER_BOOTS generations. This is
        // the only place staleness is acted on rather than merely ordered by,
        // and load is the right moment for it: the count only moves at boot.
        //
        // Skipped for v1 and v2 records, whose generation field did not exist
        // and reads as 0 - maximally stale. Applying the cut-off to those would
        // empty the table on the one upgrade the versioned loader exists to
        // carry reservations across.
        //
        // A stamp from the future - impossible, but flash is flash - reads as
        // age 0 and is kept, rather than wrapping to a huge age and being
        // dropped. Anything genuinely stale will be dropped on the next boot
        // anyway once it has been rewritten with a sane generation.
        if (hdr.version >= 3) {
            uint32_t age = (s_boot_seq > s.last_seen_seq) ? (s_boot_seq - s.last_seen_seq) : 0;
            if (age > FORGET_AFTER_BOOTS) {
                esp_ip4_addr_t pretty = { .addr = (s.ip != 0) ? s.ip : s.observed_ip };
                char ip_str[16];
                esp_ip4addr_ntoa(&pretty, ip_str, sizeof(ip_str));
                ESP_LOGI(TAG, "forgetting %s for %02x:%02x:%02x:%02x:%02x:%02x - not seen for "
                              "%u boots", ip_str, s.mac[0], s.mac[1], s.mac[2], s.mac[3],
                         s.mac[4], s.mac[5], (unsigned)age);
                forgotten++;
                continue;
            }
        }

        // Restored entries start expired: the reservation is honoured when the
        // client comes back for it, but until then the address is available to
        // be evicted rather than held out of the pool forever.
        s_leases[n].in_use = true;
        memcpy(s_leases[n].mac, s.mac, 6);
        s_leases[n].ip = s.ip;
        s_leases[n].observed_ip = s.observed_ip;
        s_leases[n].saved_ip = s.ip;
        s_leases[n].saved_observed_ip = s.observed_ip;
        s_leases[n].expires_us = 0;
        s_leases[n].last_seen_seq = s.last_seen_seq;
        s_leases[n].last_seen_us = 0;
        n++;
    }
    // A restored self-assigned address that something else in the table also
    // claims is the fossil of a duplicate-address argument. It takes two shapes:
    // the address is another entry's lease, or two entries both remember taking
    // it for themselves - which is what a duplicate settles into once both
    // devices have been seen on it and both mappings have reached flash.
    //
    // Neither may be inherited. A lease comes back from flash expired and
    // claiming nothing, while a self-assigned address comes back claiming
    // everything, so restoring the argument does not merely preserve it - it
    // hands the address to whichever side had been cut off, and renumbers the
    // one that kept it. And a claim from flash is only ever hearsay: it says
    // what this device believed last boot, not where anything is now.
    //
    // So the disputed halves go and the server keeps only its own allocation
    // record, which is the half it can vouch for. Both sides are dropped when
    // both claimed it, because neither is more authoritative than the other -
    // decided from the values as they were read, so the first one cleared does
    // not stop the second being recognised. If the devices really are still on
    // one address, both re-earn the claim from live traffic within seconds and
    // the conflict is decided fresh.
    bool drop_observed[DHCP_SERVER_MAX_LEASES] = { false };
    for (int i = 0; i < n; i++) {
        if (s_leases[i].observed_ip == 0) {
            continue;
        }
        for (int j = 0; j < n; j++) {
            if (j == i) {
                continue;
            }
            if (s_leases[j].ip == s_leases[i].observed_ip ||
                s_leases[j].observed_ip == s_leases[i].observed_ip) {
                drop_observed[i] = true;
                break;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        if (!drop_observed[i]) {
            continue;
        }
        esp_ip4_addr_t pretty = { .addr = s_leases[i].observed_ip };
        char ip_str[16];
        esp_ip4addr_ntoa(&pretty, ip_str, sizeof(ip_str));
        ESP_LOGW(TAG, "%02x:%02x:%02x:%02x:%02x:%02x was saved using %s, which another "
                      "client claims too - forgetting that, a restart clears a "
                      "duplicate address",
                 s_leases[i].mac[0], s_leases[i].mac[1], s_leases[i].mac[2],
                 s_leases[i].mac[3], s_leases[i].mac[4], s_leases[i].mac[5], ip_str);
        s_leases[i].observed_ip = 0;
        // saved_observed_ip deliberately left as what flash still holds, so the
        // mismatch marks the table dirty and the blob is rewritten without it
        // rather than carrying the fossil to the boot after.
        mark_dirty_locked();
    }

    // Entries dropped above are still in the blob, so the table has to be
    // written back without them. Deliberately only when something was actually
    // forgotten: a quiet boot must not cost a flash write.
    if (forgotten > 0) {
        mark_dirty_locked();
    }

    s_restored_count = n;
    ESP_LOGI(TAG, "restored %d MAC->IP reservation%s from flash (generation %u)%s",
             n, n == 1 ? "" : "s", (unsigned)s_boot_seq,
             forgotten > 0 ? ", forgetting clients not seen for five boots" : "");
}

// Caller must hold s_mutex.
static void save_to_nvs_locked(void)
{
    uint8_t buf[sizeof(stored_hdr_t) + DHCP_SERVER_MAX_LEASES * sizeof(stored_lease_t)];
    stored_hdr_t hdr = {
        .magic = STORE_MAGIC,
        .version = STORE_VERSION,
        .count = 0,
        // Recording this boot's generation is what lets the next boot start one
        // past it, and what gives entries not seen this time a measurably older
        // stamp than the ones written below.
        .boot_seq = s_boot_seq,
    };

    size_t off = sizeof(hdr);
    for (int i = 0; i < DHCP_SERVER_MAX_LEASES; i++) {
        // An entry with no lease but an observed address is a client that
        // addressed itself and never asked this server for anything. That
        // mapping is exactly the one worth remembering, so it is not skipped.
        //
        // Tested against what will actually be written rather than against the
        // live fields: a cut-off client whose only address is the disputed one
        // has nothing left to store, so it leaves the blob altogether.
        uint32_t observed = persisted_observed_ip(&s_leases[i]);
        if (!s_leases[i].in_use || (s_leases[i].ip == 0 && observed == 0)) {
            continue;
        }
        stored_lease_t s;
        memcpy(s.mac, s_leases[i].mac, 6);
        s.ip = s_leases[i].ip;
        s.observed_ip = observed;
        s.last_seen_seq = s_leases[i].last_seen_seq;
        memcpy(buf + off, &s, sizeof(s));
        off += sizeof(s);
        hdr.count++;
    }
    memcpy(buf, &hdr, sizeof(hdr));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_TABLE_KEY, buf, off);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }

    if (err != ESP_OK) {
        // Left dirty by the caller on failure, so the next debounce retries.
        ESP_LOGE(TAG, "failed to save leases: %s", esp_err_to_name(err));
        return;
    }

    // Only now do the saved_* fields describe flash. Entries that were dropped
    // from the table are gone from the blob too, so nothing needs clearing.
    // Recording what was written, not what is in RAM - for a cut-off client
    // those differ, and copying the live value would leave the entry looking
    // permanently unsaved and re-arming the debounce on every pass.
    for (int i = 0; i < DHCP_SERVER_MAX_LEASES; i++) {
        if (s_leases[i].in_use) {
            s_leases[i].saved_ip = s_leases[i].ip;
            s_leases[i].saved_observed_ip = persisted_observed_ip(&s_leases[i]);
        }
    }
    s_dirty = false;
    ESP_LOGI(TAG, "saved %u MAC->IP reservation%s to flash",
             (unsigned)hdr.count, hdr.count == 1 ? "" : "s");
}

// ---- address bookkeeping (all callers hold s_mutex) ----

static uint32_t ip_offset(uint32_t net_ip, uint32_t delta)
{
    return lwip_htonl(lwip_ntohl(net_ip) + delta);
}

// Membership of one served range, rather than of "the pool": there are two of
// them now, one per bridge port, and every caller below means the range the
// request in hand is being served from. Passing it in rather than reading a
// global is what keeps that choice made in exactly one place.
static bool ip_in_range(uint32_t net_ip, uint32_t start, uint32_t end)
{
    uint32_t v = lwip_ntohl(net_ip);
    return net_ip != 0 && start != 0 && v >= lwip_ntohl(start) && v <= lwip_ntohl(end);
}

static bool ip_declined(uint32_t net_ip)
{
    for (int i = 0; i < s_declined_count; i++) {
        if (s_declined[i] == net_ip) {
            return true;
        }
    }
    return false;
}

static lease_t *find_by_mac_locked(const uint8_t mac[6])
{
    for (int i = 0; i < DHCP_SERVER_MAX_LEASES; i++) {
        if (s_leases[i].in_use && memcmp(s_leases[i].mac, mac, 6) == 0) {
            return &s_leases[i];
        }
    }
    return NULL;
}

// Matches on the leased address only. Used when clearing the previous holder
// of an address being handed out - an entry must not be torn down because of
// an address it was merely seen using.
static lease_t *find_by_ip_locked(uint32_t net_ip)
{
    for (int i = 0; i < DHCP_SERVER_MAX_LEASES; i++) {
        if (s_leases[i].in_use && s_leases[i].ip == net_ip) {
            return &s_leases[i];
        }
    }
    return NULL;
}

// Matches an address claimed either way. An address a client assigned itself
// is one this server never leased, so nothing in the allocation bookkeeping
// would otherwise know it is spoken for - and handing it to somebody else
// would put two hosts on it.
//
// A quarantined client is the exception: it is on that address precisely
// because it took one that was already somebody else's, and its traffic is
// being dropped for it. Letting its claim stand here would reserve the address
// for the device that lost the argument, and after a reboot - when leases come
// back expired but observed addresses come back intact - it would be handed
// back to it outright, reversing the decision every power cycle.
static lease_t *find_by_any_ip_locked(uint32_t net_ip)
{
    for (int i = 0; i < DHCP_SERVER_MAX_LEASES; i++) {
        if (s_leases[i].in_use && !s_leases[i].quarantined &&
            (s_leases[i].ip == net_ip || s_leases[i].observed_ip == net_ip)) {
            return &s_leases[i];
        }
    }
    return NULL;
}

// True if somebody other than except is really on this address: seen using it,
// or holding an unexpired lease for it. An expired lease is not "in use" - it
// is exactly what gets recycled - but an observed address always is.
//
// A quarantined client holds nothing, for the reason given on
// find_by_any_ip_locked(): its claim is the one that lost.
static bool ip_in_use_locked(uint32_t net_ip, const lease_t *except)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < DHCP_SERVER_MAX_LEASES; i++) {
        lease_t *e = &s_leases[i];
        if (!e->in_use || e == except || e->quarantined) {
            continue;
        }
        if (e->observed_ip == net_ip) {
            return true;
        }
        if (e->ip == net_ip && e->expires_us >= now) {
            return true;
        }
    }
    return false;
}

// True if this entry has been heard from since the device booted. A restored
// entry has not - whatever it says about the client, that is evidence from a
// previous boot, and the client may have been gone for a year.
static bool seen_this_boot(const lease_t *e)
{
    return e->last_seen_seq >= s_boot_seq;
}

// Orders two candidates for reclaiming: older generation first, and within the
// same generation the one heard from longest ago.
static bool older_than(const lease_t *a, const lease_t *b)
{
    if (a->last_seen_seq != b->last_seen_seq) {
        return a->last_seen_seq < b->last_seen_seq;
    }
    return a->last_seen_us < b->last_seen_us;
}

// The address inside [start, end] that reclaiming this entry would release, or 0
// if it holds none. A client sitting on a self-assigned address outside the range
// occupies a table slot but no address the caller can use, so it is no use to a
// caller that needs an address to hand out - though it is still fair game to a
// caller that only needs the slot.
//
// The range matters now that there are two of them: an entry parked on a WiFi
// address releases nothing a wired client can be given, and reclaiming it to
// serve one would cost a working client its address and still leave the wired
// client without one.
static uint32_t reclaimable_address(const lease_t *e, uint32_t start, uint32_t end)
{
    if (ip_in_range(e->ip, start, end)) {
        return e->ip;
    }
    if (ip_in_range(e->observed_ip, start, end)) {
        return e->observed_ip;
    }
    return 0;
}

// Chooses the entry to give up when something else needs the room. Nothing here
// is a timer - see s_boot_seq for why elapsed time is not measurable, and note
// that a lease expiring means the address may go to someone else, not that the
// client should be forgotten. A laptop closed overnight outlives the 7200s lease
// and must still get its address back. The only thing that drops an entry for
// being old is FORGET_AFTER_BOOTS, counted in boots and applied at load.
//
// Two passes, because a client that set its own address is the one most likely
// to still be sitting on it and the one least able to be told otherwise:
//
//   1. plain leases whose lease has lapsed
//   2. only if none of those exist, entries with a self-assigned address that
//      have not been heard from this boot
//
// Anything heard from since boot is never a victim in either pass. Pass a
// need_address of true to require the victim release a usable address from
// [start, end]; a caller that only wants the table slot passes false, and start
// and end are then ignored.
static lease_t *pick_victim_locked(bool need_address, uint32_t start, uint32_t end,
                                    uint32_t *out_ip)
{
    int64_t now = esp_timer_get_time();

    for (int pass = 0; pass < 2; pass++) {
        lease_t *best = NULL;
        uint32_t best_ip = 0;

        for (int i = 0; i < DHCP_SERVER_MAX_LEASES; i++) {
            lease_t *e = &s_leases[i];
            if (!e->in_use || seen_this_boot(e)) {
                continue;
            }
            bool manual = (e->observed_ip != 0);
            if (manual != (pass == 1)) {
                continue;
            }
            // A live lease is in use whatever generation it was stamped in.
            if (!manual && e->expires_us >= now) {
                continue;
            }
            uint32_t ip = reclaimable_address(e, start, end);
            if (need_address && (ip == 0 || ip_declined(ip))) {
                continue;
            }
            if (best == NULL || older_than(e, best)) {
                best = e;
                best_ip = ip;
            }
        }

        if (best != NULL) {
            if (out_ip != NULL) {
                *out_ip = best_ip;
            }
            return best;
        }
    }
    return NULL;
}

// Drops an entry to make room, logging what was given up. Caller holds s_mutex.
static void reclaim_locked(lease_t *e, const char *why)
{
    esp_ip4_addr_t pretty = { .addr = (e->ip != 0) ? e->ip : e->observed_ip };
    char ip_str[16];
    esp_ip4addr_ntoa(&pretty, ip_str, sizeof(ip_str));
    ESP_LOGW(TAG, "%s - reclaiming %s from %02x:%02x:%02x:%02x:%02x:%02x, "
                  "last seen %u generation(s) ago", why, ip_str,
             e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
             (unsigned)(s_boot_seq - e->last_seen_seq));

    e->in_use = false;
    if (e->saved_ip != 0 || e->saved_observed_ip != 0) {
        mark_dirty_locked();
    }
}

// Says once, and then rarely, that the wired range is full and a client that
// asked over the cable is being served from the WiFi range instead. Rate-limited
// because a client that cannot get an address retries every few seconds, and a
// fault that fills the log ring buries the diagnosis it is trying to deliver -
// the same reason the quarantine logging in client_track.c caps its repeats.
static void note_wired_range_full(const uint8_t mac[6])
{
    static int64_t last_us;
    int64_t now = esp_timer_get_time();
    if (last_us != 0 && (now - last_us) < WIRED_FULL_LOG_US) {
        return;
    }
    last_us = now;

    char start_str[16], end_str[16];
    esp_ip4_addr_t s = { .addr = s_wired_start }, e = { .addr = s_wired_end };
    esp_ip4addr_ntoa(&s, start_str, sizeof(start_str));
    esp_ip4addr_ntoa(&e, end_str, sizeof(end_str));
    ESP_LOGW(TAG, "the wired range %s - %s is full - %02x:%02x:%02x:%02x:%02x:%02x asked over "
                  "the cable and is being served from the general pool instead",
             start_str, end_str, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Picks the address to offer or acknowledge to mac. Returns 0 if there is
// nothing to give, in which case the caller stays quiet or NAKs.
//
// The order of these four steps is the whole feature: the stored reservation
// is consulted before anything sequential, so which client asks first stops
// deciding who gets which address.
//
// wired says the DHCP message being answered arrived over the cable rather than
// over the air - see client_track_mac_on_wired_port(), which is the only thing
// that can still tell, the bridge having merged the two ports long before this.
// It chooses which range is served, and nothing below it needs to know: the
// steps are the same four they always were, run against one range or the other.
//
// A client that changes ports is therefore renumbered, and that is the intent
// rather than a side effect. Step 1 only confirms a stored reservation that
// belongs to the range now in play, so a laptop moved from the cable to the air
// is NAKed off its wired address and comes back on a WiFi one.
static uint32_t select_address_locked(const uint8_t mac[6], uint32_t requested, bool wired)
{
    lease_t *mine = find_by_mac_locked(mac);

    uint32_t start = s_pool_start, end = s_pool_end;
    if (wired && s_wired_start != 0) {
        start = s_wired_start;
        end = s_wired_end;
    }

    // 1. This MAC's reservation, if it is still a usable address. Held even
    //    while the client is away, which is what survives the reboot. Skipped
    //    if somebody else has since taken the address for real.
    //
    //    Also skipped for a quarantined client, and that is how a duplicate
    //    address gets fixed by itself rather than by hand. The client asks to
    //    renew the address it is being cut off for, this declines to confirm
    //    it, step 3 finds it a free one, and the DHCPREQUEST path NAKs the old
    //    one - which sends the client back to DISCOVER, where it takes the new
    //    address. Only works on a client that speaks DHCP at all: one that was
    //    given a static address by hand never asks, so there is nothing to
    //    answer and it stays cut off until somebody reconfigures it.
    if (mine && !mine->quarantined && ip_in_range(mine->ip, start, end) &&
        !ip_declined(mine->ip) && !ip_in_use_locked(mine->ip, mine)) {
        return mine->ip;
    }

    // 2. What the client asked for, if nobody else is on it. Covers a client
    //    that remembers its own last address after our flash was cleared.
    if (ip_in_range(requested, start, end) && !ip_declined(requested) &&
        !ip_in_use_locked(requested, mine)) {
        return requested;
    }

    // 3. First address in the range nobody has an entry for at all.
    uint32_t span = lwip_ntohl(end) - lwip_ntohl(start);
    for (uint32_t i = 0; i <= span; i++) {
        uint32_t cand = ip_offset(start, i);
        // The bridge's own address is skipped rather than assumed to be outside
        // the range: a range widened over it later should cost nothing rather
        // than hand out an address that is already spoken for.
        if (cand == s_server_ip || ip_declined(cand)) {
            continue;
        }
        if (find_by_any_ip_locked(cand) == NULL) {
            return cand;
        }
    }

    // 4. Range exhausted: take the address off whichever entry has gone longest
    //    without being heard from, oldest generation first.
    uint32_t reclaimed = 0;
    lease_t *victim = pick_victim_locked(true, start, end, &reclaimed);
    if (victim != NULL) {
        reclaim_locked(victim, "pool exhausted");
        return reclaimed;
    }

    // Nothing left in the range asked for. A wired client falls back to the
    // general pool rather than going away empty: eight addresses is a choice
    // about tidiness, and no address at all is a device that does not work.
    // Deliberately one-way - the general pool never spills into the wired range,
    // which would put WiFi stations in it and undo the point of having one.
    if (wired && start == s_wired_start && s_wired_start != 0) {
        note_wired_range_full(mac);
        return select_address_locked(mac, requested, false);
    }

    return 0;
}

// Claims a table slot for mac: a free one, or the room made by reclaiming the
// entry that has gone longest without being heard from. Returns NULL only when
// every slot belongs to a client seen this boot. Caller holds s_mutex.
static lease_t *take_free_slot_locked(const uint8_t mac[6])
{
    lease_t *e = NULL;
    for (int i = 0; i < DHCP_SERVER_MAX_LEASES; i++) {
        if (!s_leases[i].in_use) {
            e = &s_leases[i];
            break;
        }
    }
    if (e == NULL) {
        // Only the slot is needed here, not an allocatable address, so a client
        // parked on an address outside the pool is a fair candidate too.
        e = pick_victim_locked(false, 0, 0, NULL);
        if (e == NULL) {
            return NULL;
        }
        reclaim_locked(e, "lease table full");
    }

    memset(e, 0, sizeof(*e));
    e->in_use = true;
    memcpy(e->mac, mac, 6);
    return e;
}

// Records that mac holds ip for the next hold_s seconds. Marks the table dirty
// only when the *mapping* changed - a renewal of the same address writes
// nothing to flash. Returns false if there was no slot to record it in.
static bool commit_lease_locked(const uint8_t mac[6], uint32_t ip, uint32_t hold_s, bool persist)
{
    int64_t now = esp_timer_get_time();

    lease_t *e = find_by_mac_locked(mac);
    if (e == NULL) {
        e = take_free_slot_locked(mac);
        if (e == NULL) {
            // Every slot held by a client heard from this boot - so this really
            // is 32 live clients, not 32 ghosts. The address is still handed
            // out, it just won't be remembered across a reboot.
            ESP_LOGW(TAG, "lease table full (%d entries) - %02x:%02x:%02x:%02x:%02x:%02x "
                          "will not keep its address across a reboot",
                     DHCP_SERVER_MAX_LEASES, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            return false;
        }
    }

    // An address moving to a different MAC has to clear the old holder, or two
    // entries claim it and whichever is found first wins the next lookup.
    // Dropping one that was in flash is itself a change worth writing.
    lease_t *clash = find_by_ip_locked(ip);
    if (clash && clash != e) {
        clash->in_use = false;
        if (clash->saved_ip != 0) {
            mark_dirty_locked();
        }
    }

    e->ip = ip;
    e->expires_us = now + (int64_t)hold_s * 1000000;
    e->last_seen_seq = s_boot_seq;
    e->last_seen_us = now;

    // Against saved_ip, not against the previous e->ip: the address is already
    // in e->ip by the time the REQUEST confirms an OFFER made moments earlier,
    // and comparing the two would make every new client look unchanged and
    // never reach flash.
    if (persist && e->saved_ip != e->ip) {
        mark_dirty_locked();
    }
    return true;
}

// ---- option parsing and building ----

static const uint8_t *find_option(const dhcp_msg_t *m, size_t opts_len, uint8_t want, uint8_t *out_len)
{
    // The first four option bytes are the magic cookie, already checked.
    size_t i = 4;
    while (i + 1 < opts_len) {
        uint8_t code = m->options[i];
        if (code == OPT_END) {
            break;
        }
        if (code == 0) {   // pad
            i++;
            continue;
        }
        uint8_t len = m->options[i + 1];
        if (i + 2 + len > opts_len) {
            break;
        }
        if (code == want) {
            *out_len = len;
            return &m->options[i + 2];
        }
        i += 2 + len;
    }
    return NULL;
}

static uint8_t *put_opt_ip(uint8_t *p, uint8_t code, uint32_t net_ip)
{
    *p++ = code;
    *p++ = 4;
    memcpy(p, &net_ip, 4);
    return p + 4;
}

// The same option set ESP-IDF's server sent, so nothing downstream of the
// bridge sees a different network than it did before. IDF's option 43 vendor
// blob is ESP-specific and is not reproduced.
static uint8_t *put_offer_options(uint8_t *p, bool include_lease_time)
{
    p = put_opt_ip(p, OPT_SUBNET_MASK, s_netmask);

    if (include_lease_time) {
        *p++ = OPT_LEASE_TIME;
        *p++ = 4;
        uint32_t t = lwip_htonl(LEASE_TIME_S);
        memcpy(p, &t, 4);
        p += 4;
    }

    p = put_opt_ip(p, OPT_SERVER_ID, s_server_ip);
    // OPT_ROUTER is unconditionally this bridge. When there is no uplink it is a
    // gateway to nowhere, which is what it has always been; when there is one it
    // is the real router, because NAPT runs here.
    p = put_opt_ip(p, OPT_ROUTER, s_server_ip);

    // The upstream resolver the uplink learnt, so a client can resolve a caster's
    // name through the NAT - and this bridge's own address when there is no
    // uplink, exactly as before. wan_get_dns() reads a published snapshot: it
    // takes no lock and cannot block this task.
    //
    // Known limitation, and there is no DHCP mechanism that fixes it: a client
    // that leased while the uplink was down keeps 192.168.5.1 as its resolver
    // until it renews, up to LEASE_TIME_S/2 later. Nothing can push a new option
    // to a client mid-lease. The alternative - running a resolver of our own on
    // 192.168.5.1 and never changing this option - would cost a nineteenth lwIP
    // socket, and README.md's socket budget shows all eighteen are spoken for.
    uint32_t wan_dns = wan_get_dns();
    p = put_opt_ip(p, OPT_DNS_SERVER, wan_dns != 0 ? wan_dns : s_server_ip);
    p = put_opt_ip(p, OPT_BROADCAST, (s_server_ip & s_netmask) | ~s_netmask);

    *p++ = OPT_INTERFACE_MTU;
    *p++ = 2;
    *p++ = 0x05;
    *p++ = 0xdc;   // 1500

    *p++ = OPT_ROUTER_DISC;
    *p++ = 1;
    *p++ = 0x00;

    return p;
}

// Sanitised the same way ESP-IDF's server sanitised it, because web_server.c
// renders this straight into JSON and treats it as untrusted input.
static void copy_hostname(const uint8_t *val, uint8_t len, char *out, size_t out_size)
{
    size_t n = len;
    if (n >= out_size) {
        n = out_size - 1;
    }
    for (size_t i = 0; i < n; i++) {
        char c = (char)val[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        out[i] = ok ? c : '-';
    }
    out[n] = '\0';
}

// ---- sending ----

static void send_reply(const dhcp_msg_t *req, uint8_t type, uint32_t yiaddr, bool with_lease)
{
    dhcp_msg_t rep;
    memset(&rep, 0, sizeof(rep));

    rep.op = BOOTREPLY;
    rep.htype = req->htype;
    rep.hlen = req->hlen;
    rep.xid = req->xid;
    rep.flags = req->flags;
    rep.ciaddr = (type == DHCPACK) ? req->ciaddr : 0;
    rep.yiaddr = yiaddr;
    rep.giaddr = req->giaddr;
    memcpy(rep.chaddr, req->chaddr, sizeof(rep.chaddr));

    uint8_t *p = rep.options;
    uint32_t cookie = lwip_htonl(0x63825363);
    memcpy(p, &cookie, 4);
    p += 4;

    *p++ = OPT_MSG_TYPE;
    *p++ = 1;
    *p++ = type;

    if (type == DHCPNAK) {
        // A NAK carries nothing but the server id: there is no address to
        // describe, and RFC 2131 forbids the rest.
        p = put_opt_ip(p, OPT_SERVER_ID, s_server_ip);
    } else {
        p = put_offer_options(p, with_lease);
    }
    *p++ = OPT_END;

    size_t len = DHCP_FIXED_LEN + (size_t)(p - rep.options);
    if (len < DHCP_MIN_REPLY) {
        len = DHCP_MIN_REPLY;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = lwip_htons(DHCP_CLIENT_PORT);

    // A client that already has the address it is renewing can be reached
    // directly; one that is still in INIT has no address and no ARP entry, so
    // the reply has to be broadcast. ESP-IDF's server unicast to yiaddr behind
    // a manually inserted static ARP entry instead - broadcasting is what
    // RFC 2131 describes and needs no ARP table poking.
    if (req->ciaddr != 0 && type != DHCPNAK) {
        dst.sin_addr.s_addr = req->ciaddr;
    } else {
        dst.sin_addr.s_addr = INADDR_BROADCAST;
    }

    if (sendto(s_sock, &rep, len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        ESP_LOGW(TAG, "failed to send DHCP reply type %u (errno %d)", type, errno);
    }
}

static void post_assigned_event(const uint8_t mac[6], uint32_t ip, const char *hostname)
{
    ip_event_assigned_ip_to_client_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.esp_netif = s_br_netif;
    evt.ip.addr = ip;
    memcpy(evt.mac, mac, 6);
    if (hostname) {
        strlcpy(evt.hostname, hostname, sizeof(evt.hostname));
    }

    // client_track.c listens for this to learn a client's hostname (DHCP
    // option 12) - the only place it is available. Posting it here is what
    // keeps the client table populated now that IDF's server, which used to
    // raise it, is not running.
    esp_event_post(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, &evt, sizeof(evt), 0);
}

// ---- message handling ----

static void handle_message(dhcp_msg_t *m, size_t len)
{
    if (len < DHCP_FIXED_LEN + 4 || m->op != BOOTREQUEST || m->hlen != 6) {
        return;
    }
    uint32_t cookie;
    memcpy(&cookie, m->options, 4);
    if (cookie != lwip_htonl(0x63825363)) {
        return;
    }
    if (m->giaddr != 0) {
        // Relayed from another subnet. This is a flat L2 bridge with one
        // subnet on it, so there is nothing sensible to answer.
        return;
    }

    size_t opts_len = len - DHCP_FIXED_LEN;
    if (opts_len > sizeof(m->options)) {
        opts_len = sizeof(m->options);
    }

    uint8_t olen = 0;
    const uint8_t *v = find_option(m, opts_len, OPT_MSG_TYPE, &olen);
    if (v == NULL || olen < 1) {
        return;
    }
    uint8_t type = v[0];

    const uint8_t *mac = m->chaddr;

    uint32_t requested = 0;
    v = find_option(m, opts_len, OPT_REQUESTED_IP, &olen);
    if (v && olen >= 4) {
        memcpy(&requested, v, 4);
    } else {
        requested = m->ciaddr;
    }

    uint32_t server_id = 0;
    v = find_option(m, opts_len, OPT_SERVER_ID, &olen);
    if (v && olen >= 4) {
        memcpy(&server_id, v, 4);
    }

    char hostname[64] = { 0 };
    v = find_option(m, opts_len, OPT_HOSTNAME, &olen);
    if (v && olen > 0) {
        copy_hostname(v, olen, hostname, sizeof(hostname));
    }

    switch (type) {

    case DHCPDISCOVER: {
        // Asked before the lock is taken, not because it is slow but because it
        // reads a table client_track.c writes from the RX path; keeping it
        // outside means the two modules never hold each other's locks.
        bool wired = client_track_mac_on_wired_port(mac);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        uint32_t ip = select_address_locked(mac, requested, wired);
        if (ip != 0) {
            // Held, not persisted: an offer the client never takes up should
            // not become a reservation in flash.
            commit_lease_locked(mac, ip, OFFER_HOLD_S, false);
        }
        xSemaphoreGive(s_mutex);

        if (ip == 0) {
            ESP_LOGW(TAG, "no address available for %02x:%02x:%02x:%02x:%02x:%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            return;
        }
        send_reply(m, DHCPOFFER, ip, true);
        break;
    }

    case DHCPREQUEST: {
        if (server_id != 0 && server_id != s_server_ip) {
            // The client picked a different server. Drop whatever we were
            // holding for it rather than tying the address up for OFFER_HOLD_S.
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            lease_t *e = find_by_mac_locked(mac);
            if (e && e->expires_us != 0) {
                e->in_use = false;
            }
            xSemaphoreGive(s_mutex);
            return;
        }

        bool wired = client_track_mac_on_wired_port(mac);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        uint32_t ip = select_address_locked(mac, requested, wired);
        bool ok = (ip != 0) && (requested == 0 || requested == ip);
        // Whether this is the client *taking* an address rather than renewing
        // the one it already had. Decided under the lock, said outside it.
        bool moved = false;
        if (ok) {
            lease_t *before = find_by_mac_locked(mac);
            moved = (before == NULL || before->ip != ip);
            commit_lease_locked(mac, ip, LEASE_TIME_S, true);
        }
        xSemaphoreGive(s_mutex);

        if (moved) {
            // Which port a client was served from is otherwise invisible, and it
            // is the one thing worth knowing when an address is not what somebody
            // expected. Only on the ACK, and only when the address changed: a
            // renewal every couple of hours per client says nothing.
            esp_ip4_addr_t pretty = { .addr = ip };
            char ip_str[16];
            esp_ip4addr_ntoa(&pretty, ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "%02x:%02x:%02x:%02x:%02x:%02x asked over %s - giving it %s",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                     wired ? "the wired port" : "WiFi", ip_str);
        }

        if (!ok) {
            // The client is asking for an address this server will not give
            // it. NAK sends it back to DISCOVER, where it gets the address
            // that is actually reserved for it.
            send_reply(m, DHCPNAK, 0, false);
            return;
        }
        send_reply(m, DHCPACK, ip, true);
        post_assigned_event(mac, ip, hostname);
        break;
    }

    case DHCPDECLINE: {
        // The client ARP-probed the address we gave it and found it in use.
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        lease_t *e = find_by_mac_locked(mac);
        uint32_t bad = e ? e->ip : requested;
        if (bad != 0 && !ip_declined(bad) && s_declined_count < MAX_DECLINED) {
            s_declined[s_declined_count++] = bad;
        }
        if (e) {
            e->in_use = false;
            if (e->saved_ip != 0) {
                mark_dirty_locked();
            }
        }
        xSemaphoreGive(s_mutex);
        esp_ip4_addr_t pretty = { .addr = bad };
        char ip_str[16];
        esp_ip4addr_ntoa(&pretty, ip_str, sizeof(ip_str));
        ESP_LOGW(TAG, "%02x:%02x:%02x:%02x:%02x:%02x declined %s - it is already in use",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ip_str);
        break;
    }

    case DHCPRELEASE: {
        // Give the address back to the pool but keep the reservation, so a
        // client that releases on shutdown still gets the same address when
        // it comes back. Setting expires_us to 0 leaves the entry looking
        // exactly like one restored from flash: honoured for this MAC, and
        // available to be reclaimed if the pool runs dry.
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        lease_t *e = find_by_mac_locked(mac);
        if (e) {
            e->expires_us = 0;
            // Also stop counting as seen this boot. A release is the client
            // saying it has finished with the address, so the entry should be
            // reclaimable straight away if the pool comes under pressure -
            // otherwise an explicit goodbye would protect it until the next
            // reboot, which is backwards. One generation back rather than zero,
            // so it does not jump the queue ahead of genuinely older entries.
            if (s_boot_seq > 0) {
                e->last_seen_seq = s_boot_seq - 1;
            }
        }
        xSemaphoreGive(s_mutex);
        break;
    }

    case DHCPINFORM:
        // The client configured itself and only wants the options. No address
        // and no lease time, per RFC 2131.
        if (m->ciaddr != 0) {
            send_reply(m, DHCPACK, 0, false);
        }
        break;

    default:
        break;
    }
}

// ---- task ----

static void dhcp_server_task(void *arg)
{
    static dhcp_msg_t rx;   // 548 bytes - too big for this task's stack

    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(s_sock, &rx, sizeof(rx), 0, (struct sockaddr *)&src, &slen);
        if (n > 0) {
            handle_message(&rx, (size_t)n);
        }

        // Outside the receive path so a client that never speaks again still
        // gets its mapping written. The socket has a 1s receive timeout, which
        // is what paces this.
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_dirty && (esp_timer_get_time() - s_dirty_since_us) > SAVE_DEBOUNCE_US) {
            save_to_nvs_locked();   // clears s_dirty only if the write succeeded
        }
        xSemaphoreGive(s_mutex);
    }
}

// ---- netif binding ----

// Pins DHCP traffic to the bridge. Without this, a reply to a client that has
// no address yet goes to 255.255.255.255, and lwIP picks the egress netif with
// ip_route(), which for a global broadcast is whatever won the default-netif
// election - the AP port (route_prio 10) during the window between
// esp_wifi_start() and the bridge coming up. A bound pcb short-circuits that:
// udp_send() consults pcb->netif_idx before it routes anything, and udp_input()
// filters arriving datagrams by the same index, which confines this server to
// the one LAN it exists to serve.
//
// Deliberately not one-shot. The bridge's struct netif is handed to lwIP by
// esp_netif_action_start() and taken back on stop, and netif_add() is what
// assigns the index, so a binding made once can end up naming a netif that no
// longer exists.
static void bind_to_bridge(bool bound)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));

    if (bound) {
        // Fails while the bridge has no lwIP netif yet, which is the case for
        // the whole of dhcp_server_start(). Leaving the socket unbound then is
        // right - the handler below binds it once the bridge reports itself up.
        if (esp_netif_get_netif_impl_name(s_br_netif, ifr.ifr_name) != ESP_OK) {
            return;
        }
    }
    // An empty name unbinds, which is what the down path wants.
    if (setsockopt(s_sock, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0) {
        ESP_LOGW(TAG, "could not bind the socket to the bridge (errno %d) - "
                      "replies will follow the default netif", errno);
        return;
    }
    ESP_LOGI(TAG, "socket %s the bridge", bound ? "bound to" : "unbound from");
}

// IP_EVENT_NETIF_UP/DOWN rather than a got-IP event: the bridge netif is
// created from ESP_NETIF_INHERENT_DEFAULT_BR_DHCPS(), whose get_ip_event is 0,
// so there is no IP_EVENT_*_GOT_IP raised for it to hook. These two are posted
// from esp_netif's own lwIP ext-status callback whenever a netif's effective
// state flips, for every netif, which is why the handler has to filter.
static void br_netif_status_cb(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    const ip_event_netif_status_t *evt = data;
    if (evt->esp_netif != s_br_netif) {
        return;
    }
    bind_to_bridge(id == IP_EVENT_NETIF_UP);
}

esp_err_t dhcp_server_start(esp_netif_t *br_netif,
                            esp_ip4_addr_t ip, esp_ip4_addr_t netmask,
                            esp_ip4_addr_t pool_start, esp_ip4_addr_t pool_end,
                            esp_ip4_addr_t wired_start, esp_ip4_addr_t wired_end)
{
    if (br_netif == NULL || ip.addr == 0 || pool_start.addr == 0 || pool_end.addr == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lwip_ntohl(pool_start.addr) > lwip_ntohl(pool_end.addr)) {
        ESP_LOGE(TAG, "pool start is above pool end");
        return ESP_ERR_INVALID_ARG;
    }

    s_br_netif = br_netif;
    s_server_ip = ip.addr;
    s_netmask = netmask.addr;
    s_pool_start = pool_start.addr;
    s_pool_end = pool_end.addr;

    // A wired range that runs backwards, overlaps the general pool, swallows the
    // bridge's own address or sits on another subnet is a mistake in a
    // compile-time constant. Complained about and switched off rather than
    // refused: main.c calls this inside ESP_ERROR_CHECK, so returning an error
    // here would turn a wrong constant into a device that panics on boot and
    // cannot be reached to be fixed. A bridge that serves one range to everybody
    // is a far better failure than one that serves nothing.
    s_wired_start = wired_start.addr;
    s_wired_end = wired_end.addr;
    if (s_wired_start != 0) {
        uint32_t ws = lwip_ntohl(s_wired_start), we = lwip_ntohl(s_wired_end);
        uint32_t ps = lwip_ntohl(s_pool_start), pe = lwip_ntohl(s_pool_end);
        bool bad = (s_wired_end == 0) || (ws > we) ||
                   (ws <= pe && ps <= we) ||                       // overlaps the pool
                   ip_in_range(s_server_ip, s_wired_start, s_wired_end) ||
                   ((s_wired_start & s_netmask) != (s_server_ip & s_netmask)) ||
                   ((s_wired_end & s_netmask) != (s_server_ip & s_netmask));
        if (bad) {
            char s_str[16], e_str[16];
            esp_ip4addr_ntoa(&wired_start, s_str, sizeof(s_str));
            esp_ip4addr_ntoa(&wired_end, e_str, sizeof(e_str));
            ESP_LOGE(TAG, "wired range %s - %s overlaps the pool or the bridge, runs backwards, "
                          "or is off-subnet - ignoring it and serving one range to every port, "
                          "fix ETH_DHCP_START/ETH_DHCP_END in main.c", s_str, e_str);
            s_wired_start = 0;
            s_wired_end = 0;
        }
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    load_from_nvs();

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed (errno %d)", errno);
        return ESP_FAIL;
    }

    int on = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    // Paces the save-debounce check in the task loop, so a dirty table is
    // written within a second or so of the debounce expiring even when no
    // further DHCP traffic arrives.
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;   // must be ANY to receive broadcasts
    bind_addr.sin_port = lwip_htons(DHCP_SERVER_PORT);
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        // The likeliest cause is ESP-IDF's own DHCP server still holding the
        // port, which means the bridge netif was created with
        // ESP_NETIF_DHCP_SERVER in its flags. See setup_bridge() in main.c.
        ESP_LOGE(TAG, "bind to port %d failed (errno %d) - is another DHCP server running?",
                 DHCP_SERVER_PORT, errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    // Nothing above needed a netif - INADDR_ANY binds without one. The socket
    // is pinned to the bridge separately, whenever the bridge comes up.
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_NETIF_UP,
                                               br_netif_status_cb, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_NETIF_DOWN,
                                               br_netif_status_cb, NULL));
    // Expected to do nothing today, since the bridge netif is not started until
    // esp_eth_start()/esp_wifi_start() raise the port events that start it. It
    // is here so this module does not care where in app_main it is called from,
    // rather than trading one ordering assumption for another.
    bind_to_bridge(true);

    // 5120 rather than 4096: send_reply() builds a 548-byte message on the
    // stack, and the same task also runs the NVS write.
    BaseType_t ok = xTaskCreatePinnedToCore(dhcp_server_task, "dhcp_server", 5120,
                                            NULL, tskIDLE_PRIORITY + 2, NULL, 1);
    if (ok != pdPASS) {
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    char start_str[16], end_str[16];
    char wired_str[34] = "none";
    esp_ip4addr_ntoa(&pool_start, start_str, sizeof(start_str));
    esp_ip4addr_ntoa(&pool_end, end_str, sizeof(end_str));
    if (s_wired_start != 0) {
        esp_ip4_addr_t ws = { .addr = s_wired_start }, we = { .addr = s_wired_end };
        char a[16], b[16];
        esp_ip4addr_ntoa(&ws, a, sizeof(a));
        esp_ip4addr_ntoa(&we, b, sizeof(b));
        snprintf(wired_str, sizeof(wired_str), "%s - %s", a, b);
    }
    ESP_LOGI(TAG, "started - wifi %s - %s, wired %s, %d reservation%s restored from flash",
             start_str, end_str, wired_str, s_restored_count,
             s_restored_count == 1 ? "" : "s");
    return ESP_OK;
}

void dhcp_server_note_observed_ip(const uint8_t mac[6], esp_ip4_addr_t ip)
{
    if (s_mutex == NULL || ip.addr == 0) {
        return;
    }

    // Only addresses on this bridge's own subnet. The bridge is transparent,
    // so frames from hosts on entirely different subnets legitimately cross
    // it, and a client that fails DHCP and self-assigns a 169.254 link-local
    // is not telling us its address either. Recording those would be wrong,
    // and would rewrite flash every time the client changed its mind.
    if ((ip.addr & s_netmask) != (s_server_ip & s_netmask) || ip.addr == s_server_ip) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    lease_t *e = find_by_mac_locked(mac);
    if (e == NULL) {
        // A client that addressed itself and never spoke DHCP still gets an
        // entry, so the association is remembered across a reboot - reclaiming
        // a stale one if that is what it takes, rather than silently dropping
        // a device that is demonstrably on the network.
        e = take_free_slot_locked(mac);
        if (e == NULL) {
            xSemaphoreGive(s_mutex);
            return;
        }
    }

    // Nothing to record while the client is using exactly what it was leased;
    // clearing it keeps a stale manual address from outliving a return to DHCP.
    uint32_t want = (ip.addr == e->ip) ? 0 : ip.addr;
    if (e->observed_ip != want) {
        if (want != 0) {
            esp_ip4_addr_t pretty = { .addr = want };
            char ip_str[16];
            esp_ip4addr_ntoa(&pretty, ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "%02x:%02x:%02x:%02x:%02x:%02x is using %s, which this "
                          "server did not lease it - remembering that",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ip_str);
        }
        e->observed_ip = want;
    }
    // Traffic from this client is what makes it "seen this boot", which is what
    // protects it from being reclaimed out from under itself.
    e->last_seen_seq = s_boot_seq;
    e->last_seen_us = esp_timer_get_time();

    if (e->saved_observed_ip != persisted_observed_ip(e)) {
        mark_dirty_locked();
    }
    xSemaphoreGive(s_mutex);
}

void dhcp_server_set_quarantined(const uint8_t (*macs)[6], int n)
{
    if (s_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < DHCP_SERVER_MAX_LEASES; i++) {
        lease_t *e = &s_leases[i];
        if (!e->in_use) {
            e->quarantined = false;
            continue;
        }
        bool found = false;
        for (int j = 0; j < n; j++) {
            if (memcmp(e->mac, macs[j], 6) == 0) {
                found = true;
                break;
            }
        }
        e->quarantined = found;

        // Arming changes what this entry should look like in flash, and so does
        // disarming. The disputed address is usually already written by the
        // time a conflict is confirmed - the save debounce is 10s and arming
        // takes 3 - so without this the blob would keep the claim until some
        // unrelated change happened to rewrite it, which is exactly the state
        // that must not survive a restart.
        if (e->saved_observed_ip != persisted_observed_ip(e)) {
            mark_dirty_locked();
        }
    }
    xSemaphoreGive(s_mutex);
}

bool dhcp_server_lookup_ip(const uint8_t mac[6], esp_ip4_addr_t *out_ip)
{
    if (s_mutex == NULL) {
        return false;
    }
    bool found = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    lease_t *e = find_by_mac_locked(mac);
    if (e && e->observed_ip != 0) {
        // What the client was last seen actually using beats what it was
        // leased - the same precedence client_track.c applies to a sniffed
        // source address. After a reboot this is what lets a manually
        // addressed client show up correctly before it has sent anything.
        out_ip->addr = e->observed_ip;
        found = true;
    } else if (e && e->ip != 0 && e->expires_us > 0) {
        // expires_us 0 means a reservation nobody has claimed yet. Reporting
        // that would put an address against a MAC that isn't using it.
        out_ip->addr = e->ip;
        found = true;
    }
    xSemaphoreGive(s_mutex);
    return found;
}

int dhcp_server_get_leases(dhcp_lease_info_t *out, int max)
{
    if (s_mutex == NULL) {
        return 0;
    }
    int n = 0;
    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < DHCP_SERVER_MAX_LEASES && n < max; i++) {
        lease_t *e = &s_leases[i];
        if (!e->in_use || (e->ip == 0 && e->observed_ip == 0)) {
            continue;
        }
        memcpy(out[n].mac, e->mac, 6);
        out[n].ip.addr = e->ip;
        out[n].observed_ip.addr = e->observed_ip;
        out[n].expires_in_s = (e->expires_us > now)
                                  ? (uint32_t)((e->expires_us - now) / 1000000)
                                  : 0;
        out[n].boots_since_seen = seen_this_boot(e) ? 0 : (s_boot_seq - e->last_seen_seq);
        out[n].saved_ip.addr = e->saved_ip;
        out[n].saved_observed_ip.addr = e->saved_observed_ip;
        // "Is flash up to date", which for a cut-off client means flash
        // correctly holding *nothing* for the disputed address - not a pending
        // write that will never come.
        out[n].stored = (e->saved_ip == e->ip &&
                         e->saved_observed_ip == persisted_observed_ip(e));
        n++;
    }
    xSemaphoreGive(s_mutex);
    return n;
}

int dhcp_server_get_restored_count(void)
{
    return s_restored_count;
}
