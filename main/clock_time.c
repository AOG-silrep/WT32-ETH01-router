#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include "clock_time.h"
#include "clock_cfg.h"
#include "reset_log.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_netif.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "clock_time";

// The fallbacks, used when the upstream network offers no NTP server of its own.
// There are two, and the second exists because the first has a dependency that
// fails in the field.
//
// A pool NAME first: the pool rotates, so it survives any individual server being
// retired, and resolving it costs a DNS lookup the WAN filter already permits.
#define NTP_FALLBACK "pool.ntp.org"

// Then a literal ADDRESS, for the case that turns out to be common on the
// networks this device actually lands on: an upstream router that routes fine but
// whose DNS resolver does not answer. Every name-based server is equally useless
// there - they all need the same broken resolver - so a second hostname would add
// nothing. Only an address gets a clock onto a device behind a router like that.
//
// 162.159.200.123 is time.cloudflare.com. Hard-coding an address is normally a
// bet on one machine outliving the device, which is why the name comes first;
// this one is anycast, so it is a bet on a globally announced prefix rather than
// on a host, and those are the terms that make it acceptable as a last resort. It
// is tried only after the two above, so a network with working DNS never reaches
// it.
#define NTP_FALLBACK_IP "162.159.200.123"

// Past this without a correction, the surfaces stop presenting the clock as
// exact. Set against the oscillator rather than against taste: the RTC runs on
// the internal RC (CONFIG_RTC_CLK_SRC_INT_RC), whose drift over temperature runs
// to hundreds of parts per million, which is of the order of a minute a day. So
// seconds stop meaning anything long before the timestamp stops being useful,
// and a day is where "to the second" clearly no longer holds. Nothing here
// invalidates a stale clock - a reset dated to the right hour is still worth
// having - it only stops the surfaces implying a precision that has gone.
#define CLOCK_STALE_AFTER_S (24 * 60 * 60)

// ---- RTC state ----

#define CLOCK_TIME_RTC_MAGIC 0x434C4B31  // 'CLK1'

// Complement pairs and a separate magic gate, copied from reset_log_rtc_t and
// for the reasons stated there: RTC slow memory comes up uninitialised rather
// than zeroed, so a bare flag would read as "synchronised" out of power-on
// garbage roughly half the time - and the whole point of this flag is that it
// is the only thing standing between a garbage clock and a dated reset record.
//
// Kept in its own block rather than added to reset_log's, deliberately. RTC
// memory survives an OTA restart, so growing that struct would make the first
// boot after a firmware update find a bad magic and lose the previous boot's
// exact uptime - an evidence loss on precisely the boot an operator is most
// likely to be watching. A separate block costs a handful of the 8KB and avoids
// it entirely.
typedef struct {
    uint32_t magic;
    uint32_t synced,  synced_inv;    // a real sync has happened since power-on
    uint32_t sync_lo, sync_lo_inv;   // when, as time_t, split for the pairs
    uint32_t sync_hi, sync_hi_inv;
} clock_time_rtc_t;

static RTC_NOINIT_ATTR clock_time_rtc_t s_rtc;

// Set by the sync callback in the tcpip task, read by the tick that performs the
// backfill. Not mutex-guarded: a 32-bit aligned store is atomic on this core,
// there is exactly one writer, and the worst a race could do is delay the
// backfill by one tick.
static volatile bool s_synced_this_boot;
static bool s_backfilled;
static bool s_started;

// For the log, not for logic: paired with the wall time in RTC, this is what
// turns two syncs into a drift figure. Boot-local and genuinely not derivable -
// the RTC keeps the wall time across a reset but not this boot's uptime at it.
static int64_t s_last_sync_up_s;   // this boot's uptime at the last sync
static int64_t s_started_at_us;    // esp_timer at the last clock_time_wan_up()
static bool s_warned_unsynced;
static bool s_resolved;    // the one-shot name check has been run this boot

// How long to wait before saying out loud that nothing has answered. Long enough
// that a slow DHCP lease and a first DNS lookup are not reported as a fault, and
// short enough that somebody watching the console after configuring a WAN sees
// the verdict rather than giving up first.
#define SYNC_WARN_AFTER_S 90

static bool rtc_valid(void)
{
    return s_rtc.magic == CLOCK_TIME_RTC_MAGIC &&
           s_rtc.synced  == ~s_rtc.synced_inv &&
           s_rtc.sync_lo == ~s_rtc.sync_lo_inv &&
           s_rtc.sync_hi == ~s_rtc.sync_hi_inv;
}

static void rtc_note_sync(time_t when)
{
    uint64_t w = (uint64_t)when;
    s_rtc.sync_lo = (uint32_t)(w & 0xffffffffu);
    s_rtc.sync_lo_inv = ~s_rtc.sync_lo;
    s_rtc.sync_hi = (uint32_t)(w >> 32);
    s_rtc.sync_hi_inv = ~s_rtc.sync_hi;
    s_rtc.synced = 1;
    s_rtc.synced_inv = ~s_rtc.synced;
    // Magic last, so a reset landing mid-update leaves the block invalid rather
    // than half-written and believed.
    s_rtc.magic = CLOCK_TIME_RTC_MAGIC;
}

static time_t rtc_last_sync(void)
{
    if (!rtc_valid() || s_rtc.synced == 0) {
        return 0;
    }
    return (time_t)(((uint64_t)s_rtc.sync_hi << 32) | (uint64_t)s_rtc.sync_lo);
}

// ---- the clock ----

// Static: every caller is in this file. Kept as a named predicate rather than
// inlined because three places ask the same question and the answer is a
// two-part condition.
static bool clock_time_synced(void)
{
    return rtc_valid() && s_rtc.synced != 0;
}

bool clock_time_now(time_t *out)
{
    if (!clock_time_synced()) {
        return false;
    }
    time_t now = time(NULL);
    // A clock that passed the flag check but reads before the flag was written
    // is not a clock, it is RTC memory that survived with a corrupt companion.
    // Cheap to check and it closes the one hole the complement pairs cannot.
    time_t last = rtc_last_sync();
    if (last != 0 && now < last) {
        return false;
    }
    if (out != NULL) {
        *out = now;
    }
    return true;
}

bool clock_time_stale(void)
{
    time_t now, last = rtc_last_sync();
    if (!clock_time_now(&now) || last == 0) {
        return false;   // no clock at all is not the same claim as a drifting one
    }
    return (now - last) > CLOCK_STALE_AFTER_S;
}

time_t clock_time_last_sync(void)
{
    return rtc_last_sync();
}

const char *clock_time_source(void)
{
    if (!clock_time_synced()) {
        return "none";
    }
    return s_synced_this_boot ? "ntp" : "carried";
}

void clock_time_format(time_t t, char *out, size_t outsz)
{
    struct tm tm;
    if (localtime_r(&t, &tm) == NULL) {
        strlcpy(out, "?", outsz);
        return;
    }
    strftime(out, outsz, "%Y-%m-%d %H:%M:%S", &tm);
}

// %Z through strftime rather than struct tm's tm_zone, which is a BSD extension
// this libc exposes only under the right feature macros. The buffer is static
// because callers pass the result straight into a printf; it is only ever
// written from the httpd worker and the console task, and both would print the
// same string anyway.
const char *clock_time_zone_abbrev(time_t t)
{
    static char abbrev[8];
    struct tm tm;
    if (localtime_r(&t, &tm) == NULL) {
        return "";
    }
    if (strftime(abbrev, sizeof(abbrev), "%Z", &tm) == 0) {
        return "";
    }
    return abbrev;
}

// ---- SNTP ----

// Renders one configured slot for the log: the hostname if it was set by name,
// the address if it was handed over by DHCP (which supplies an address, never a
// name), or NULL when the slot is empty. Slot 0 is DHCP's, slot 1 the compiled
// fallback - see the config in clock_time_init().
static const char *server_desc(uint8_t idx, char *buf, size_t bufsz)
{
    const char *name = esp_sntp_getservername(idx);
    if (name != NULL && name[0] != '\0') {
        return name;
    }
    const ip_addr_t *ip = esp_sntp_getserver(idx);
    if (ip == NULL || ip_addr_isany(ip)) {
        return NULL;
    }
    strlcpy(buf, ipaddr_ntoa(ip), bufsz);
    return buf;
}

// Every configured slot, on one line, saying where each came from. This is the
// line that answers "is it even trying the right server", which is the first
// question after "why is the clock not set".
//
// Iterates the whole table rather than naming a fixed pair, because the number of
// slots is a build setting (CONFIG_LWIP_SNTP_MAX_SERVERS) and a loop that knew
// about two of three once already hid the slot that mattered.
//
// Slot 0 is the one DHCP overwrites when the upstream network offers option 42,
// and telling the two cases apart matters: DHCP hands over an ADDRESS, never a
// name, so a hostname still sitting in slot 0 means nothing was offered and the
// compiled-in fallback is what is there.
static void log_servers(const char *why)
{
    char line[160];
    int off = 0;
    for (uint8_t i = 0; i < CONFIG_LWIP_SNTP_MAX_SERVERS; i++) {
        char b[48];
        const char *d = server_desc(i, b, sizeof(b));
        unsigned reach = 0;
        esp_netif_sntp_reachability(i, &reach);
        int n = snprintf(line + off, sizeof(line) - off, "%s[%u] %s r=%u",
                         i ? ", " : "", (unsigned)i, d ? d : "-", reach);
        if (n < 0 || (size_t)(off + n) >= sizeof(line)) {
            break;
        }
        off += n;
    }
    const char *n0 = esp_sntp_getservername(0);
    bool from_dhcp = (n0 == NULL || n0[0] == '\0') && server_desc(0, (char[48]){0}, 48) != NULL;
    ESP_LOGI(TAG, "%s: %s (slot 0 %s; r is lwIP's reachability register)", why, line,
             from_dhcp ? "offered by the upstream network"
                       : "built-in default, none offered");
}

// Runs in the tcpip task. Everything expensive is deferred to clock_time_tick():
// the backfill writes to flash, and reset_log.h states the rule this follows -
// an NVS write must come from an ordinary task, never from a callback that the
// network stack is waiting on.
static void on_sync(struct timeval *tv)
{
    time_t when = (tv != NULL) ? tv->tv_sec : time(NULL);
    int64_t up_s = esp_timer_get_time() / 1000000;

    // The correction, which is the number worth having and is only computable
    // from the second sync onwards. Predict where the clock should have got to
    // on its own since the last sync, and report the difference: that is the
    // oscillator's drift over a known interval, and it is the one measurement
    // that says whether a clock reported as good actually is. On the first sync
    // there is nothing to compare against - the clock was arbitrary.
    char drift[64] = "";
    if (s_synced_this_boot) {
        // rtc_last_sync() still holds the PREVIOUS sync here - rtc_note_sync()
        // below has not run yet - so there is no separate copy to keep.
        time_t predicted = (time_t)(rtc_last_sync() + (up_s - s_last_sync_up_s));
        long off = (long)(when - predicted);
        snprintf(drift, sizeof(drift), ", %+ld s off over the last %ld s",
                 off, (long)(up_s - s_last_sync_up_s));
    }

    rtc_note_sync(when);
    s_last_sync_up_s = up_s;
    s_synced_this_boot = true;

    // Rendered here rather than left to the caller: an epoch in the log is a
    // number somebody has to go and convert, and this line is the one a person
    // reads to confirm the feature works at all.
    // Through this module's own exported formatter, not a second copy of it: the
    // header calls that function "the single formatter every surface goes
    // through", and a hand-rolled strftime here would have made the claim false
    // in the one file that makes it.
    char now_str[CLOCK_TIME_STR_MAX];
    clock_time_format(when, now_str, sizeof(now_str));
    char b[48];
    const char *from = server_desc(0, b, sizeof(b));
    // Slot 0 is what lwIP asks first; naming it makes "which server is this time
    // coming from" answerable without a packet capture.
    ESP_LOGI(TAG, "clock set to %s %s from %s%s", now_str,
             clock_time_zone_abbrev(when), from ? from : "the configured server", drift);
}

// Says whether this network can resolve a name, once, and exits.
//
// It exists because the two ways the clock fails are indistinguishable from
// everything else in the log: a resolver that does not answer and a network that
// drops NTP both leave the clock unset with a reachability of zero. One line
// here separates them, and on the networks this device actually lands on the
// first is the common one - an upstream router that routes fine while its DNS
// answers nothing.
//
// One attempt, not several. Retrying was worth it while the failure was unknown;
// now that a literal-address fallback exists (NTP_FALLBACK_IP), a name that will
// not resolve is no longer fatal to the clock, so this is a diagnostic rather
// than a recovery path and does not need to keep trying. lwIP's SNTP does its own
// resolution and its own retries regardless.
//
// Its own task rather than a step in clock_time_tick(), because getaddrinfo()
// blocks for as long as the resolver takes to fail - seconds, on exactly the
// network where this question gets asked - and that tick shares the 1 Hz loop
// that samples CPU load and writes the uptime checkpoint. Stalling that to run a
// diagnostic would corrupt the numbers on the page somebody is reading while they
// wait for the answer.
static void resolve_task(void *arg)
{
    // Which resolver lwIP will actually ask, which is not necessarily the one
    // the WAN was handed: lwIP keeps one global DNS table, not a per-netif one.
    const ip_addr_t *d0 = dns_getserver(0);
    ESP_LOGI(TAG, "resolver in use: %s",
             (d0 && !ip_addr_isany(d0)) ? ipaddr_ntoa(d0) : "none");

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    int64_t t0 = esp_timer_get_time();
    int err = getaddrinfo(NTP_FALLBACK, "123", &hints, &res);
    long ms = (long)((esp_timer_get_time() - t0) / 1000);

    if (err == 0 && res != NULL) {
        char ip[16] = "?";
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        esp_ip4_addr_t a = { .addr = sa->sin_addr.s_addr };
        esp_ip4addr_ntoa(&a, ip, sizeof(ip));
        ESP_LOGI(TAG, "%s resolves to %s (%ld ms) - name resolution works, so a clock "
                      "that still does not set means NTP itself is being dropped upstream",
                 NTP_FALLBACK, ip, ms);
        freeaddrinfo(res);
    } else {
        ESP_LOGW(TAG, "cannot resolve %s (error %d after %ld ms) - this network's resolver "
                      "is not answering. The clock falls back to %s, which needs no DNS, so "
                      "it should still set; anything else on the LAN that resolves names "
                      "will not work until the upstream resolver does.",
                 NTP_FALLBACK, err, ms, NTP_FALLBACK_IP);
    }
    vTaskDelete(NULL);
}

esp_err_t clock_time_init(void)
{
    // Invalidated rather than trusted on a power-on boot. esp_reset_reason() is
    // not consulted: the complement pairs already answer "did anything write
    // here", and a power-on wipes the registers IDF keeps the clock's origin in
    // anyway, so a surviving flag with no clock behind it is the case to guard
    // against and rtc_valid() plus the sanity check in clock_time_now() do it.
    if (!rtc_valid()) {
        memset(&s_rtc, 0, sizeof(s_rtc));
        s_rtc.synced_inv = ~s_rtc.synced;
        s_rtc.sync_lo_inv = ~s_rtc.sync_lo;
        s_rtc.sync_hi_inv = ~s_rtc.sync_hi;
        s_rtc.magic = CLOCK_TIME_RTC_MAGIC;
    }

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST(NTP_FALLBACK, NTP_FALLBACK_IP));
    // Nothing goes out until the WAN says it has an address. Started here and
    // the client would poll into a filter that drops it, inflating tx_blocked -
    // a counter the WAN panel presents as evidence of a misconfigured port list.
    cfg.start = false;
    cfg.wait_for_sync = false;
    cfg.sync_cb = on_sync;
    // The upstream network's own NTP server, when it offers one in the DHCP ACK,
    // is a better answer than a public pool: it is closer, it is what everything
    // else on that network agrees with, and reaching it needs no DNS. lwip puts
    // it at index 0, which is why the fallbacks are moved aside to index 1 and 2
    // rather than being overwritten by it - that is what these three fields do,
    // on the GOT_IP that carries the option. CONFIG_LWIP_SNTP_MAX_SERVERS is 3
    // for exactly this: DHCP's, the pool name, and the literal address.
    cfg.server_from_dhcp = true;
    cfg.renew_servers_after_new_IP = true;
    cfg.index_of_first_server = 1;
    cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP unavailable: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

void clock_time_wan_up(void)
{
    if (s_started) {
        return;
    }
    esp_err_t err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not start the NTP client: %s", esp_err_to_name(err));
        return;
    }
    s_started = true;
    s_started_at_us = esp_timer_get_time();
    s_warned_unsynced = false;
    ESP_LOGI(TAG, "asking the network what time it is");
    log_servers("clock");

    // 4KB covers getaddrinfo() and the log call; priority 3 is below the
    // forwarding path and above idle, matching the other housekeeping here.
    if (!s_resolved) {
        s_resolved = true;
        if (xTaskCreate(resolve_task, "ntp_resolve", 4096, NULL, 3, NULL) != pdPASS) {
            ESP_LOGW(TAG, "could not start the NTP name check");
        }
    }
}

void clock_time_wan_down(void)
{
    if (!s_started) {
        return;
    }
    // Stopped rather than left polling. With no route the requests would be
    // refused by this device's own egress filter, and every refusal lands on
    // tx_blocked - the counter the WAN panel offers as evidence that the port
    // list is wrong. A diagnostic that climbs on its own is worse than no
    // diagnostic.
    esp_sntp_stop();
    s_started = false;
    // Logged, because the clock going quiet explains a "carried"/stale reading
    // later and is otherwise invisible - the WAN's own down message says
    // nothing about what stopped alongside it.
    ESP_LOGI(TAG, "NTP client stopped with the WAN%s",
             s_synced_this_boot ? "; the clock keeps running from here on its own"
                                : " before it ever got an answer");
}

void clock_time_tick(void)
{
    // The silence case, said out loud once. Without this a WAN that is up but
    // whose network blocks NTP produces exactly one line - "asking the network
    // what time it is" - and then nothing forever, which reads as the feature
    // never having run rather than as it having been refused. Said once rather
    // than repeatedly: it is a standing condition, not an event, and a warning
    // every minute for the life of a boot trains people to ignore the log.
    if (s_started && !s_synced_this_boot && !s_warned_unsynced &&
        (esp_timer_get_time() - s_started_at_us) > (int64_t)SYNC_WARN_AFTER_S * 1000000) {
        s_warned_unsynced = true;
        // Reachability is lwIP's own eight-bit shift register of recent
        // REPLIES, so zero means nothing has come back - it does not say
        // whether anything went out, because the register is shifted rather
        // than incremented on send and shifting zero leaves zero. The line that
        // separates "never sent" from "sent and ignored" is the DNS result
        // logged by resolve_task() above; this one says how it ended.
        ESP_LOGW(TAG, "no NTP answer after %d s - this boot's reset record will have no "
                      "date, and the clock stays unset", SYNC_WARN_AFTER_S);
        // The per-slot reachability rides along in this line, so the two are read
        // together rather than one naming servers and the other numbering them.
        log_servers("still trying");
    }

    // Derived rather than latched: "a sync happened and the record is not yet
    // stamped" is exactly these two flags, and a separate pending flag only
    // added a race between setting and consuming it.
    if (s_backfilled || !s_synced_this_boot) {
        return;
    }

    time_t now;
    if (!clock_time_now(&now)) {
        return;
    }
    // When this boot STARTED, not when the clock arrived. The record is about a
    // reset, and the reset happened within a second or so of the boot beginning;
    // stamping the sync instead would date it however long the WAN took to
    // associate, which on a cold start is minutes and on a flapping uplink is
    // unbounded.
    int64_t up_s = esp_timer_get_time() / 1000000;
    reset_log_note_time((time_t)(now - up_s));

    // Set whether or not the write landed. reset_log_note_time() swallows its
    // own NVS failures by design, and retrying every hourly re-sync for the life
    // of the boot would spend flash on a write that is already failing.
    s_backfilled = true;
}
