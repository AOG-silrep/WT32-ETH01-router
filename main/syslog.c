#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "syslog.h"
#include "syslog_cfg.h"
#include "log_buf.h"
#include "reset_log.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/socket.h>
#include <net/if.h>
#include "lwip/sockets.h"

static const char *TAG = "syslog";

// How long the task sleeps when nothing wakes it. Every decision the loop makes
// is re-derived from state on each pass - the generation counter for the
// configuration, s_bound for the link, the cursor against the ring's range for
// new lines - so this timeout is what makes all of them correct even if a
// notification is missed entirely. The notifications only make them fast.
#define SYSLOG_IDLE_TICK_MS 1000

// Datagrams per pass before the task yields. A boot backlog is up to
// LOG_BUF_LINES (128), and a tight loop of that many sendto() calls allocates
// that many PBUF_RAM buffers out of the same heap the forwarding path is using -
// on a device whose whole reason for existing is forwarding. 16 per 20ms caps
// the sender at 800 lines/s, which still drains a full ring in 160ms and is
// orders of magnitude more than this firmware can generate.
#define SYSLOG_BURST_MAX 16
#define SYSLOG_PACE_MS   20

// How many passes in a row may end in "out of buffers" before the sender stops
// believing it. ENOMEM from sendto() is genuinely transient under momentary
// memory pressure - but it is also what lwIP returns forever when the
// collector's address never answers ARP, because the packet sits in the ARP
// queue until that fills too. The two are indistinguishable from here, and
// treating the second as the first is a sender that retries one line at the
// pacing rate for as long as the address stays wrong: measured at 50 attempts a
// second against a configured-but-absent collector, with the backlog never
// draining and every later line ageing out of the ring behind it.
//
// So it is transient three times, and after that it is a property of the
// configuration and the line is dropped like any other permanent failure. A
// single successful send resets the count, so real memory pressure - which ends
// - costs a few retries and nothing else.
#define SYSLOG_STALL_MAX 3

// One line per state change, and never more than one a minute even then - the
// same limit and the same reasoning as AUTH_FAIL_LOG_INTERVAL_MS in
// web_server.c. A cable working itself loose flips WAITING/SHIPPING at whatever
// rate the PHY reports it, and unmetered that is a line per flap into a
// 128-line ring, erasing the record of the flapping that somebody is reading
// the log to understand.
#define SYSLOG_LOG_MIN_INTERVAL_MS 60000

// 384, against a measured worst case of 334 bytes plus the NUL snprintf writes:
// every field at its maximum - a 32-character hostname, a 31-character tag, a
// 143-character message, and all four counters at ten digits. The 49 bytes of
// slack are so a field added to the structured data later does not require this
// arithmetic to be redone - the same reason LOGS_TRAILER_MAX in web_server.c is
// rounded up past its own measured value.
//
// Comfortably under the 480 bytes RFC 5426 requires every receiver to accept,
// so no collector can refuse one of these for size, and far under the 1500-byte
// MTU so nothing fragments.
#define SYSLOG_DGRAM_MAX 384

// What the sender is doing, for the one line a minute it is allowed to say
// about itself. Ordered by how much attention each deserves.
typedef enum {
    SYSLOG_OFF,       // disabled in configuration
    SYSLOG_WAITING,   // enabled, bridge down - holding the backlog
    SYSLOG_SHIPPING,  // enabled, bound, sends succeeding
    SYSLOG_FAILING,   // enabled, bound, sendto() returning errors
} syslog_state_t;

static esp_netif_t *s_br_netif;
static TaskHandle_t s_task;
static int s_sock = -1;
static bool s_bound;

static syslog_cfg_t s_cfg;
static uint32_t s_seen_generation;
static struct sockaddr_in s_dest;

// The last sequence number handed to lwIP. Identical in meaning to the "since"
// cursor logs_get_handler() serves, and 0 means the same thing there as here: a
// reader that has seen nothing, which starts from whatever the ring still
// holds. That is what makes the first pass ship every line logged before this
// module existed on this boot.
static uint32_t s_cursor;

static uint32_t s_sent, s_filtered, s_aged_out, s_send_fail;
static int s_last_errno;
static uint32_t s_boot_seq;

// Whether the most recent datagram this sender attempted failed. Last event
// wins, so a sender that failed an hour ago and has succeeded since reads as
// shipping rather than as permanently broken - the counters are what remember
// the history, this only says what is true now.
static bool s_last_send_failed;

// Consecutive passes that ended out of buffers, and whether the pass that just
// ran was one of them. The flag keeps the task from re-draining immediately: a
// stalled pass has nothing to gain from being retried 20ms later, and going back
// to the notification wait drops the retry rate from the pacing interval to the
// idle tick.
static uint32_t s_stalls;
static bool s_pass_stalled;

// State-transition logging, all touched only by the syslog task.
static syslog_state_t s_logged_state = SYSLOG_OFF;
static bool s_ever_logged;
static TickType_t s_last_log_tick;
static uint32_t s_suppressed;

// Static rather than a stack local: SYSLOG_DGRAM_MAX is a meaningful share of
// this task's stack, and only this one task ever touches it - the same trade
// log_buf.c makes for s_staging and dhcp_server.c for its receive buffer.
static char s_dgram[SYSLOG_DGRAM_MAX];

// ---- RFC 5424 framing ----

// RFC 5424 section 6.2.1 severities, for each level letter log_buf stores.
//
// '?' is the interesting one. log_buf maps an unparsed line to ESP_LOG_ERROR
// (log_buf.c's level_from_letter()), but that is a decision about a *console
// threshold*: "we could not classify this, so err towards showing it." Here the
// severity is a claim about the line rather than a decision about it, and
// calling an unparsed WiFi-driver fragment an error would be a false one.
//
// Notice instead. This firmware never emits Notice deliberately, so on the
// collector the level means exactly one thing - "log_buf could not read a level
// off this line" - and it sits above Informational, so it survives a minimum
// severity set anywhere at or below info. An unclassifiable line is the last
// one worth dropping quietly.
static int severity_from_letter(char letter)
{
    switch (letter) {
        case 'E': return 3;   // Error
        case 'W': return 4;   // Warning
        case 'I': return 6;   // Informational
        case 'D': return 7;   // Debug
        case 'V': return 7;   // Debug - RFC 5424 has nothing below it
        default:  return 5;   // Notice - see above
    }
}

// APP-NAME is PRINTUSASCII (%d33-126), 1-48 characters (RFC 5424 section
// 6.2.5). log_buf's tags are at most LOG_BUF_TAG_MAX-1 characters so the length
// is already safe, but the bytes are not: a tag is whatever sat before the first
// colon on a line this firmware did not necessarily author, and an unparsed line
// has no tag at all.
//
// An empty tag becomes the NILVALUE. A zero-length APP-NAME is not a legal
// field, and a collector that accepts one usually does so by silently eating the
// field that follows - which here would be PROCID, quietly relabelling the boot
// number as the application name.
static void sanitize_appname(const char *tag, char *out, size_t outsz)
{
    if (tag[0] == '\0') {
        strlcpy(out, "-", outsz);
        return;
    }
    size_t i = 0;
    for (const unsigned char *p = (const unsigned char *)tag; *p != '\0' && i + 1 < outsz; p++) {
        out[i++] = (*p >= 33 && *p <= 126) ? (char)*p : '_';
    }
    out[i] = '\0';
}

// No BOM is written. RFC 5424 section 6.4 lets a leading BOM declare MSG to be
// UTF-8, and this device cannot honestly make that declaration: a DHCP hostname
// or an SSID reaches the ring as whatever bytes the client put on the wire.
// Without the BOM the encoding is formally unknown, which is the true answer,
// and every collector treats it as bytes either way.
//
// Only the control characters are replaced, and only because a raw newline or
// NUL inside MSG splits or truncates the record at more than one collector.
// log_buf already folds newlines to spaces and strips ANSI, so this is the belt
// to that braces - a control byte can still arrive inside a device-supplied
// string. Bytes above 0x7e pass through untouched, so a message that really is
// UTF-8 arrives intact. The replacement is one byte for one, so nothing grows.
static void sanitize_msg(char *s)
{
    for (unsigned char *p = (unsigned char *)s; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            *p = '.';
        }
    }
}

// timeQuality is RFC 5424 section 7.1's reserved SD-ID - used without an
// enterprise number - and it is the standards-blessed way to say "this device
// is not stamping these against a synchronised clock", which is the entire
// reason TIMESTAMP is the NILVALUE. Still accurate now that a WAN can supply a
// clock: these lines are deliberately not dated from it (see syslog.h), so
// isSynced="0" is the honest claim about this datagram whatever the device
// happens to know. It costs 38 bytes to make that self-
// explaining to a collector operator who never read this firmware's README.
//
// aog@32473 uses PEN 32473, the enterprise number IANA documents for examples
// and private use (RFC 5612). up_ms is the ring's own millisecond stamp, seq is
// the ring sequence number, boot is the boot counter. Together they order every
// line the collector ever receives from this device, across reboots, without
// depending on a clock at either end.
//
// boot duplicates PROCID deliberately: PROCID is where a collector indexes it,
// this is where a person reads it, next to the two numbers it is only meaningful
// alongside. Ten bytes.
#define SYSLOG_SD_FMT \
    "[timeQuality tzKnown=\"0\" isSynced=\"0\"]" \
    "[aog@32473 up_ms=\"%u\" seq=\"%u\" boot=\"%u\"]"

// ---- socket ----

// Pins the sender to the bridge, and re-pins it every time the bridge starts or
// stops. Deliberately not one-shot, for the reason dhcp_server.c's copy of this
// gives: the bridge's struct netif is handed to lwIP by esp_netif_action_start()
// and taken back on stop, and netif_add() is what assigns the index, so a
// binding made once can end up naming a netif that no longer exists.
//
// It matters more here than it does for DHCP. The collector's address is on the
// bridge's own subnet, so an unbound socket routes by ip_route() and can hand
// the datagram to the AP port during the window between esp_wifi_start() and the
// bridge coming up - the log would leave by an interface that is not the one it
// was configured for, if it left at all.
static bool bind_to_bridge(bool bound)
{
    if (s_sock < 0) {
        return false;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));

    if (bound) {
        // Fails while the bridge has no lwIP netif yet. Reporting that as
        // "not bound" is right - the event handler binds it once the bridge
        // reports itself up.
        if (esp_netif_get_netif_impl_name(s_br_netif, ifr.ifr_name) != ESP_OK) {
            return false;
        }
    }
    // An empty name unbinds, which is what the down path wants.
    if (setsockopt(s_sock, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0) {
        s_last_errno = errno;
        return false;
    }
    return bound;
}

static void open_socket(void)
{
    if (s_sock >= 0) {
        return;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        s_last_errno = errno;
        return;
    }

    // No bind(). This socket only ever sends, so binding would take a local port
    // on the LAN for nothing - and on a device whose lwIP descriptor table is
    // sized to the byte (see the accounting on max_open_sockets in
    // web_server.c), "for nothing" is not free.
    //
    // SO_BROADCAST is deliberately absent. syslog_cfg_validate() rejects the
    // broadcast address, and leaving the option off means a configuration that
    // somehow slipped past it fails loudly at sendto() rather than quietly
    // spraying every log line at every host on the segment.
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    s_bound = bind_to_bridge(true);
}

static void close_socket(void)
{
    if (s_sock < 0) {
        return;
    }
    bind_to_bridge(false);
    close(s_sock);
    s_sock = -1;
    s_bound = false;
}

// Unlike dhcp_server.c's equivalent, this handler does not log. It runs on the
// system event task, which gets CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE (2304)
// bytes and has no way to rate-limit itself, so a flapping netif would put a
// line per flap into the ring from a path that cannot help it. The transition is
// reported by settle_log() on the task instead, under the interval above.
static void br_netif_status_cb(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    const ip_event_netif_status_t *evt = data;
    if (evt->esp_netif != s_br_netif) {
        return;
    }
    s_bound = bind_to_bridge(id == IP_EVENT_NETIF_UP);
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

// ---- the drain ----

// Nothing in here logs. Not on a send failure, not on an aged-out line, not on
// anything: every error path sets a counter and s_last_errno, and that is the
// whole of it.
//
// Two reasons, and both are hard requirements rather than preferences. The first
// is the feedback loop - this module's own ESP_LOGx lines land in the ring and
// are then shipped by this same function, so one line per failed datagram would
// write a line, which becomes a line to send, which fails, which writes a line.
// That fills all 128 slots in well under a second and erases the history
// somebody is reading the log to find. The second is log_buf.h's own rule that a
// caller must not log inside a sequence of ring calls.
//
// Returns true while lines remain to send.
static bool drain_pass(void)
{
    uint32_t oldest = 0, newest = 0, missed = 0;
    log_buf_get_range(&oldest, &newest, &missed);

    uint32_t next = s_cursor + 1;
    if (next < oldest) {
        // Lines that left the ring before they could be sent. Counted rather
        // than skipped silently, for the reason logs_get_handler() reports
        // "lost": advancing over a hole and saying nothing turns a gap into a
        // splice. The only ways to reach this are a link that was down while the
        // ring wrapped, or a burst faster than this sender's pacing. A cursor of
        // 0 is a fresh start, which has missed nothing.
        if (s_cursor != 0) {
            s_aged_out += oldest - next;
        }
        next = oldest;
    }

    int budget = SYSLOG_BURST_MAX;
    uint32_t seq = next;
    for (; seq <= newest && budget > 0; seq++) {
        char level = '?';
        uint32_t ts_ms = 0;
        char tag[LOG_BUF_TAG_MAX];
        char msg[LOG_BUF_MSG_MAX];

        // Copied out before framing, never while holding the ring's lock - the
        // same rule logs_get_handler() follows, and for the same reason.
        if (!log_buf_get_line(seq, &level, &ts_ms, tag, sizeof(tag), msg, sizeof(msg))) {
            // Overwritten between the range read and here: the ring's lock is
            // dropped between lines, so a burst can wrap past a pass still in
            // progress.
            s_aged_out++;
            s_cursor = seq;
            continue;
        }

        int severity = severity_from_letter(level);
        if (severity > s_cfg.min_severity) {
            // Counted separately from aged_out so a deliberate filter never
            // reads as loss - the same distinction /api/logs draws between
            // "lost" and "missed".
            s_filtered++;
            s_cursor = seq;
            continue;
        }

        char appname[LOG_BUF_TAG_MAX];
        sanitize_appname(tag, appname, sizeof(appname));
        sanitize_msg(msg);

        int len = snprintf(s_dgram, sizeof(s_dgram),
                           "<%u>1 - %s %s %u - " SYSLOG_SD_FMT " %s",
                           (unsigned)(s_cfg.facility * 8 + severity),
                           s_cfg.hostname, appname, (unsigned)s_boot_seq,
                           (unsigned)ts_ms, (unsigned)seq, (unsigned)s_boot_seq,
                           msg);
        // Cannot happen given the arithmetic on SYSLOG_DGRAM_MAX, but clamp
        // rather than trust it: a truncated datagram is worth more than a
        // dropped one, and snprintf has already written a terminated string.
        if (len < 0) {
            s_cursor = seq;
            continue;
        }
        if (len >= (int)sizeof(s_dgram)) {
            len = (int)sizeof(s_dgram) - 1;
        }

        int n = sendto(s_sock, s_dgram, (size_t)len, 0,
                       (struct sockaddr *)&s_dest, sizeof(s_dest));
        budget--;
        s_last_send_failed = (n < 0);
        if (n < 0) {
            s_send_fail++;
            s_last_errno = errno;
            // Out of buffers says nothing about this line, so the first few
            // times it happens the cursor stays put and the line is retried.
            // After SYSLOG_STALL_MAX passes it is treated like any other
            // failure and the cursor advances - see the note there for why
            // retrying it indefinitely is worse than dropping it.
            //
            // Every other errno is a property of the configuration or the stack
            // rather than of this line, and retrying one forever would freeze
            // the cursor while the ring wrapped past it, turning one bad
            // datagram into the loss of all the history behind it.
            if ((errno == ENOMEM || errno == ENOBUFS) && s_stalls < SYSLOG_STALL_MAX) {
                s_stalls++;
                s_pass_stalled = true;
                break;
            }
        } else {
            s_sent++;
            s_stalls = 0;
        }
        s_cursor = seq;
    }

    return s_cursor < newest;
}

// ---- state reporting ----

static syslog_state_t current_state(void)
{
    if (!s_cfg.enabled) {
        return SYSLOG_OFF;
    }
    if (!s_bound || s_sock < 0) {
        return SYSLOG_WAITING;
    }
    return s_last_send_failed ? SYSLOG_FAILING : SYSLOG_SHIPPING;
}

// The one place this module logs, called once per pass, after the drain loop has
// finished and holding no locks. Because it runs after the loop, any line it
// writes is shipped by the *next* pass and cannot be re-triggered by the pass
// that wrote it - which bounds the feedback to at most one line per pass before
// the state and interval checks below bring it far below even that.
static void settle_log(syslog_state_t state)
{
    if (state == s_logged_state && s_ever_logged) {
        // The steady state, and by far the common one: a sender moving thousands
        // of datagrams says nothing at all about itself.
        return;
    }

    TickType_t now = xTaskGetTickCount();
    // Unsigned tick subtraction, so the window stays correct across the
    // TickType_t rollover. The flag is what makes the first transition after
    // boot print: s_last_log_tick starts at 0, which within the first minute of
    // uptime reads as "just logged".
    if (s_ever_logged &&
        (TickType_t)(now - s_last_log_tick) < pdMS_TO_TICKS(SYSLOG_LOG_MIN_INTERVAL_MS)) {
        s_suppressed++;
        return;
    }

    uint32_t also = s_suppressed;
    s_suppressed = 0;
    s_last_log_tick = now;
    s_logged_state = state;
    s_ever_logged = true;

    // 64 against a 56-byte worst case: the fixed text is 46 characters and the
    // count is at most 10 digits.
    char also_str[64] = "";
    if (also > 0) {
        // "since the last of these" rather than "in the last minute": the count
        // is carried until there is a line to print it on, so a burst that stops
        // mid-window is still reported by whatever transition comes next.
        snprintf(also_str, sizeof(also_str), " (%u more state changes since the last of these)",
                 (unsigned)also);
    }

    char server[16];
    esp_ip4_addr_t addr = { .addr = s_cfg.server_ip };
    esp_ip4addr_ntoa(&addr, server, sizeof(server));

    // Read before the ESP_LOGx calls below, never across one: log_buf_get_range()
    // takes the ring's mutex, and logging while holding it would re-enter the
    // capture hook onto a non-recursive mutex this task already held.
    uint32_t oldest = 0, newest = 0, missed = 0;
    log_buf_get_range(&oldest, &newest, &missed);
    unsigned backlog = (newest > s_cursor) ? (unsigned)(newest - s_cursor) : 0u;

    switch (state) {
        case SYSLOG_OFF:
            ESP_LOGI(TAG, "Remote logging off%s", also_str);
            break;
        case SYSLOG_WAITING:
            ESP_LOGW(TAG, "Waiting for the bridge network - holding %u line(s) for %s:%u%s",
                     backlog, server, (unsigned)s_cfg.port, also_str);
            break;
        case SYSLOG_SHIPPING:
            ESP_LOGI(TAG, "Shipping the log to %s:%u as %s%s",
                     server, (unsigned)s_cfg.port, s_cfg.hostname, also_str);
            break;
        case SYSLOG_FAILING:
            // The only place errno is ever printed. Everywhere else it is a
            // counter, because a line per failure is the feedback loop.
            ESP_LOGE(TAG, "Cannot send to %s:%u (errno %d) - %u failure(s) so far%s",
                     server, (unsigned)s_cfg.port, s_last_errno,
                     (unsigned)s_send_fail, also_str);
            break;
    }
}

// ---- configuration ----

static void reload_config(void)
{
    bool was_enabled = s_cfg.enabled;

    syslog_cfg_get(&s_cfg);
    s_seen_generation = syslog_cfg_generation();

    memset(&s_dest, 0, sizeof(s_dest));
    s_dest.sin_family = AF_INET;
    s_dest.sin_addr.s_addr = s_cfg.server_ip;
    s_dest.sin_port = lwip_htons(s_cfg.port);

    if (s_cfg.enabled && s_cfg.server_ip != 0) {
        open_socket();
        if (!was_enabled) {
            // Somebody who just turned this on did so to find out what has been
            // happening, so replay everything the ring still holds rather than
            // starting from the next line. Resetting to 0 is the same "has seen
            // nothing" the cursor starts at.
            s_cursor = 0;
        }
    } else {
        // Closing rather than idling: it returns a descriptor to a pool the web
        // server can otherwise exhaust on its own.
        close_socket();
    }
}

// ---- task ----

static void syslog_task(void *arg)
{
    reload_config();

    for (;;) {
        // Both notification sources - a committed log line and a saved
        // configuration - mean the same thing here, "wake up and do a pass", so
        // they share the default notification index and are never told apart.
        // What changed is worked out from state below, not from which give
        // arrived.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SYSLOG_IDLE_TICK_MS));

        if (syslog_cfg_generation() != s_seen_generation) {
            reload_config();
        }

        if (s_cfg.enabled && s_bound && s_sock >= 0) {
            s_pass_stalled = false;
            // A stalled pass ends the burst rather than pacing into another
            // one: there is nothing to be gained by asking again 20ms later,
            // and waiting for the next notification or idle tick turns a
            // permanently-unreachable collector from a 50-per-second retry loop
            // into a once-a-second one.
            while (drain_pass() && !s_pass_stalled) {
                vTaskDelay(pdMS_TO_TICKS(SYSLOG_PACE_MS));
            }
        }

        settle_log(current_state());
    }
}

// ---- public ----

esp_err_t syslog_init(esp_netif_t *br_netif)
{
    if (br_netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_br_netif = br_netif;

    reset_log_entry_t boot;
    if (reset_log_get_current(&boot)) {
        s_boot_seq = boot.boot_seq;
    }

    // Registered before the task starts, and before esp_eth_start()/
    // esp_wifi_start() in app_main: these events are posted from esp_netif's
    // lwIP ext-status callback for every netif whose effective state flips,
    // which is why the handler filters, and a registration made after the bridge
    // came up would never see the one that matters.
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_NETIF_UP,
                                               br_netif_status_cb, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_NETIF_DOWN,
                                               br_netif_status_cb, NULL));

    // 3072: the datagram buffer is static, so the stack carries only a line copy
    // (a tag and a message, ~180 bytes) plus lwIP's sendto() path. Priority +1
    // rather than dhcp_server's +2 - handing out addresses outranks talking
    // about them - and core 1 like every other task in this firmware.
    BaseType_t ok = xTaskCreatePinnedToCore(syslog_task, "syslog", 3072, NULL,
                                            tskIDLE_PRIORITY + 1, &s_task, 1);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    // Attached only once the task exists to be notified. Everything logged
    // before this point is still in the ring and is picked up by the first
    // pass - the notification is for keeping up, not for catching up.
    log_buf_set_notify(s_task);
    return ESP_OK;
}

void syslog_get_status(syslog_status_t *out)
{
    uint32_t oldest = 0, newest = 0, missed = 0;
    log_buf_get_range(&oldest, &newest, &missed);

    // Read without a lock. Every field is a single word written only by the
    // syslog task, so a reader can see a set of counters from either side of one
    // increment but never a torn value - and these are display counters, in the
    // same spirit as log_buf's own missed count. A mutex here would put the
    // httpd worker on the syslog task's critical path once a second for nothing.
    out->enabled    = s_cfg.enabled;
    out->bound      = s_bound;
    out->sent       = s_sent;
    out->filtered   = s_filtered;
    out->aged_out   = s_aged_out;
    out->send_fail  = s_send_fail;
    out->last_errno = s_last_errno;
    out->backlog    = (newest > s_cursor) ? (newest - s_cursor) : 0;
}

void syslog_config_changed(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void syslog_get_subnet(uint32_t *ip, uint32_t *netmask)
{
    *ip = 0;
    *netmask = 0;

    esp_netif_ip_info_t info;
    if (s_br_netif == NULL || esp_netif_get_ip_info(s_br_netif, &info) != ESP_OK) {
        return;
    }
    *ip = info.ip.addr;
    *netmask = info.netmask.addr;
}
