#include <string.h>
#include <stdio.h>
#include "serial_console.h"
#include "wifi_cfg.h"
#include "auth_cfg.h"
#include "client_track.h"
#include "dhcp_server.h"
#include "sys_monitor.h"
#include "eth_link.h"
#include "reset_log.h"
#include "clock_time.h"
#include "clock_cfg.h"
#include "log_buf.h"
#include "syslog.h"
#include "syslog_cfg.h"
#include "wan.h"
#include "wan_cfg.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "serial_console";

// ---- wifi ----

static struct {
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_int *channel;
    struct arg_end *end;
} wifi_args;

static int cmd_wifi(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&wifi_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wifi_args.end, argv[0]);
        return 1;
    }

    char cur_ssid[WIFI_CFG_SSID_MAX_LEN];
    char cur_password[WIFI_CFG_PASSWORD_MAX_LEN];
    uint8_t cur_channel;
    wifi_cfg_load(cur_ssid, cur_password, &cur_channel);

    bool any_given = wifi_args.ssid->count > 0 || wifi_args.password->count > 0 || wifi_args.channel->count > 0;
    if (!any_given) {
        printf("SSID: %s\nChannel: %u\n", cur_ssid, (unsigned)cur_channel);
        return 0;
    }

    // Any field not explicitly given keeps its current saved value, so
    // -s, -p, and -c can each be changed independently.
    const char *ssid = wifi_args.ssid->count > 0 ? wifi_args.ssid->sval[0] : cur_ssid;
    // Blank/absent password keeps the current one, same fallback the web UI
    // uses (see wifi_post_handler in web_server.c).
    const char *password = (wifi_args.password->count > 0 && wifi_args.password->sval[0][0] != '\0')
                                ? wifi_args.password->sval[0]
                                : cur_password;
    uint8_t channel = wifi_args.channel->count > 0 ? (uint8_t)wifi_args.channel->ival[0] : cur_channel;

    const char *err_msg = NULL;
    if (!wifi_cfg_validate(ssid, password, channel, &err_msg)) {
        printf("Error: %s\n", err_msg);
        return 1;
    }

    if (wifi_cfg_save(ssid, password, channel) != ESP_OK) {
        printf("Error: failed to save WiFi config\n");
        return 1;
    }

    printf("Saved. SSID: %s, Channel: %u\nRebooting to apply...\n", ssid, (unsigned)channel);
    fflush(stdout);
    // The AP is a live bridge port - same reasoning as wifi_post_handler:
    // a full reboot re-applies cleanly via the normal boot path.
    //
    // Same intent as the web form deliberately: the reset history answers "did
    // somebody mean this", and which interface they used is already in the log
    // ring alongside it.
    reset_log_note_intent(RESET_INTENT_WIFI_SAVE);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

// ---- admin ----

static struct {
    struct arg_str *user;
    struct arg_str *password;
    struct arg_end *end;
} admin_args;

static int cmd_admin(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&admin_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, admin_args.end, argv[0]);
        return 1;
    }

    char cur_user[AUTH_CFG_USERNAME_MAX_LEN];
    char cur_password[AUTH_CFG_PASSWORD_MAX_LEN];
    auth_cfg_load(cur_user, cur_password);

    if (admin_args.user->count == 0 && admin_args.password->count == 0) {
        printf("Admin username: %s\n", cur_user);
        return 0;
    }

    const char *new_user = admin_args.user->count > 0 ? admin_args.user->sval[0] : cur_user;
    const char *new_password = admin_args.password->count > 0 ? admin_args.password->sval[0] : cur_password;

    const char *err_msg = NULL;
    if (!auth_cfg_validate(new_user, new_password, &err_msg)) {
        printf("Error: %s\n", err_msg);
        return 1;
    }

    if (auth_cfg_save(new_user, new_password) != ESP_OK) {
        printf("Error: failed to save admin credentials\n");
        return 1;
    }

    printf("Saved. Admin username: %s\n", new_user);
    return 0;
}

// ---- sysinfo ----

// Defined down in the resets section, where its two helpers live. Declared here
// so the "Last restart" line below is worded by exactly the same code as the
// "resets" command, rather than by a second copy that drifts.
static void describe_reset(const reset_log_entry_t *e, char *out, size_t n);

static int cmd_sysinfo(int argc, char **argv)
{
    uint8_t cpu_pct[SYS_MONITOR_NUM_CORES];
    sys_monitor_get_cpu_load(cpu_pct);

    client_info_t clients[CLIENT_TRACK_MAX_CLIENTS];
    int count = 0;
    client_track_get_snapshot(clients, CLIENT_TRACK_MAX_CLIENTS, &count);
    uint32_t rx_total = 0, tx_total = 0;
    for (int i = 0; i < count; i++) {
        rx_total += clients[i].rx_bps;
        tx_total += clients[i].tx_bps;
    }
    uint32_t rx_pps = 0, tx_pps = 0;
    client_track_get_traffic_pps(&rx_pps, &tx_pps);

    eth_link_status_t eth;
    eth_link_get_status(&eth);

    const esp_app_desc_t *app_desc = esp_app_get_description();

    printf("Version:        %s\n", app_desc->version);
    printf("Uptime:         %llu s\n", (unsigned long long)(esp_timer_get_time() / 1000000ULL));
    // Deliberately adjacent to Uptime: the two read as one thought - "up four
    // hours, and last time it was a panic".
    reset_log_entry_t cur;
    if (reset_log_get_current(&cur)) {
        // 160, not 112: the longest wording is a 63-character "what happened",
        // " after " , a 23-character duration and the parenthesised boot number
        // and date, which is 139. 112 predates the date and was already one
        // long reason short of truncating.
        char desc[160];
        describe_reset(&cur, desc, sizeof(desc));
        printf("Last restart:   %s\n", desc);
    }
    printf("Free heap:      %u bytes\n", (unsigned)esp_get_free_heap_size());
    printf("Min free heap:  %u bytes\n", (unsigned)esp_get_minimum_free_heap_size());
    printf("CPU load:       core0 %u%%, core1 %u%%\n", (unsigned)cpu_pct[0], (unsigned)cpu_pct[1]);
    printf("CPU freq:       %u MHz\n", (unsigned)sys_monitor_get_cpu_freq_mhz());
    printf("Net traffic:    rx %u B/s (%u pkt/s), tx %u B/s (%u pkt/s)\n",
           (unsigned)rx_total, (unsigned)rx_pps, (unsigned)tx_total, (unsigned)tx_pps);
    if (eth.up) {
        printf("Eth port:       up, %u Mbit %s duplex\n",
               (unsigned)eth.speed_mbit, eth.full_duplex ? "full" : "half");
    } else {
        printf("Eth port:       DOWN\n");
    }
    printf("Eth link:       %u flap%s, last change %u s ago\n",
           (unsigned)eth.flaps, eth.flaps == 1 ? "" : "s", (unsigned)eth.since_change_s);
    printf("Clients:        %d\n", count);
    printf("Traffic drops:  %u\n", (unsigned)client_track_get_traffic_drops());
    return 0;
}

// ---- clients ----

static int cmd_clients(int argc, char **argv)
{
    client_info_t clients[CLIENT_TRACK_MAX_CLIENTS];
    int count = 0;
    client_track_get_snapshot(clients, CLIENT_TRACK_MAX_CLIENTS, &count);

    if (count == 0) {
        printf("No active clients.\n");
        return 0;
    }

    printf("%-18s %-15s %-4s %-5s %10s %10s %8s %8s %6s %s\n",
           "MAC", "IP", "LINK", "RSSI", "RX B/s", "TX B/s", "RX p/s", "TX p/s", "SEEN", "NAME");
    for (int i = 0; i < count; i++) {
        client_info_t *c = &clients[i];
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                  c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5]);
        char ip_str[16];
        esp_ip4addr_ntoa(&c->ip, ip_str, sizeof(ip_str));
        char rssi_str[8] = "-";
        if (c->is_wifi) {
            snprintf(rssi_str, sizeof(rssi_str), "%d", c->rssi);
        }
        printf("%-18s %-15s %-4s %-5s %10u %10u %8u %8u %5us %s\n",
               mac_str, ip_str, c->is_wifi ? "wifi" : "eth", rssi_str,
               (unsigned)c->rx_bps, (unsigned)c->tx_bps, (unsigned)c->rx_pps, (unsigned)c->tx_pps,
               (unsigned)c->last_seen_s, c->name);
    }
    return 0;
}

// ---- leases ----

static int cmd_leases(int argc, char **argv)
{
    dhcp_lease_info_t leases[DHCP_SERVER_MAX_LEASES];
    int count = dhcp_server_get_leases(leases, DHCP_SERVER_MAX_LEASES);

    if (count == 0) {
        printf("No DHCP leases.\n");
        return 0;
    }

    client_track_conflict_t conflicts[CLIENT_TRACK_MAX_CONFLICTS];
    int conflict_count = client_track_get_conflicts(conflicts, CLIENT_TRACK_MAX_CONFLICTS);
    bool any_conflict = false;

    printf("%-18s %-15s %9s %-13s %s\n", "MAC", "IP", "EXPIRES", "SEEN", "STATE");
    for (int i = 0; i < count; i++) {
        dhcp_lease_info_t *l = &leases[i];
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                  l->mac[0], l->mac[1], l->mac[2], l->mac[3], l->mac[4], l->mac[5]);

        // An address the client set for itself is what it is actually on, so
        // that is the one to lead with; the lease it ignored goes in the state
        // column, where it explains why the two differ.
        bool manual = (l->observed_ip.addr != 0);
        char ip_str[16];
        esp_ip4addr_ntoa(manual ? &l->observed_ip : &l->ip, ip_str, sizeof(ip_str));

        char expires_str[12] = "-";
        if (!manual && l->expires_in_s > 0) {
            snprintf(expires_str, sizeof(expires_str), "%us", (unsigned)l->expires_in_s);
        }

        // 64, not 48: the longest state is now the duplicate-address one,
        // "manual (leased 192.168.5.100), duplicate - blocked, unsaved" at 59
        // characters. strlcat truncates rather than failing, so a buffer left
        // at 48 would have silently cut the clause off instead of saying so.
        char state[64];
        if (manual && l->ip.addr != 0) {
            char leased_str[16];
            esp_ip4addr_ntoa(&l->ip, leased_str, sizeof(leased_str));
            snprintf(state, sizeof(state), "manual (leased %s)", leased_str);
        } else if (manual) {
            strlcpy(state, "manual", sizeof(state));
        } else if (l->expires_in_s > 0) {
            strlcpy(state, "active", sizeof(state));
        } else {
            // No time left: a reservation held for a client that isn't here -
            // restored from flash, or released on shutdown. It is still the
            // address that client gets back when it returns.
            strlcpy(state, "reserved", sizeof(state));
        }
        // Before ", unsaved", and worded exactly as leases.html words it: the
        // two describe one table, and a reader comparing them should never have
        // to work out whether the difference means anything.
        //
        // The other device's MAC is not named here, for the same reason it is
        // not named on the page - it made the row too wide. There it moved into
        // the tooltip; a terminal has no tooltips, so it moved to the note
        // below and to "quarantine", which prints both ends of every conflict.
        for (int j = 0; j < conflict_count; j++) {
            if (!conflicts[j].armed) {
                continue;
            }
            bool blocked = memcmp(conflicts[j].mac, l->mac, 6) == 0;
            bool holder = memcmp(conflicts[j].peer_mac, l->mac, 6) == 0;
            if (!blocked && !holder) {
                continue;
            }
            strlcat(state, blocked ? ", duplicate - blocked" : ", duplicate - kept",
                    sizeof(state));
            any_conflict = true;
            break;
        }
        if (!l->stored) {
            strlcat(state, ", unsaved", sizeof(state));
        }

        // Reclaim order, not a clock: this device cannot measure elapsed time
        // across a power cycle, so staleness is counted in boots that wrote the
        // table rather than in hours. "now" means the client has been heard
        // from since boot, which is what keeps it from being reclaimed.
        char seen_str[24];   // "4294967295 boots ago"
        if (l->boots_since_seen == 0) {
            strlcpy(seen_str, "now", sizeof(seen_str));
        } else {
            snprintf(seen_str, sizeof(seen_str), "%u boot%s ago",
                     (unsigned)l->boots_since_seen, l->boots_since_seen == 1 ? "" : "s");
        }

        printf("%-18s %-15s %9s %-13s %s\n", mac_str, ip_str, expires_str, seen_str, state);
    }

    // Mirrors the note the web page puts under the table when it is showing the
    // same rows. Only printed when there is something to point at.
    if (any_conflict) {
        printf("\nTwo devices are on one address. Run \"quarantine\" to see which "
               "device is on each,\nand to override the decision.\n");
    }
    return 0;
}

// ---- quarantine ----

static struct {
    struct arg_str *mac;
    struct arg_end *end;
} kick_args;

static struct {
    struct arg_str *action;
    struct arg_str *mac;
    struct arg_end *end;
} quarantine_args;

// "aa:bb:cc:dd:ee:ff" in any case, with '-' accepted for ':' since that is how
// Windows writes them.
static bool parse_mac(const char *s, uint8_t out[6])
{
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6 &&
        sscanf(s, "%2x-%2x-%2x-%2x-%2x-%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xff) {
            return false;
        }
        out[i] = (uint8_t)v[i];
    }
    return true;
}

static int cmd_quarantine(int argc, char **argv)
{
    int errors = arg_parse(argc, argv, (void **)&quarantine_args);
    if (errors != 0) {
        arg_print_errors(stderr, quarantine_args.end, argv[0]);
        return 1;
    }

    const char *action = (quarantine_args.action->count > 0)
                             ? quarantine_args.action->sval[0] : NULL;

    if (action != NULL && (strcmp(action, "on") == 0 || strcmp(action, "off") == 0)) {
        bool on = (strcmp(action, "on") == 0);
        client_track_quarantine_enforce(on);
        printf(on ? "Enforcing: duplicate-address offenders will be cut off again.\n"
                  : "Enforcement off: conflicts are still detected and reported, "
                    "but nothing is dropped.\n");
        return 0;
    }

    if (action != NULL && strcmp(action, "clear") == 0) {
        if (quarantine_args.mac->count == 0) {
            printf("Usage: quarantine clear <mac|all>\n");
            return 1;
        }
        const char *who = quarantine_args.mac->sval[0];
        if (strcmp(who, "all") == 0) {
            client_track_quarantine_clear(NULL);
            printf("All pardons withdrawn. Conflicts still present will re-arm "
                   "within a few seconds.\n");
            return 0;
        }
        uint8_t mac[6];
        if (!parse_mac(who, mac)) {
            printf("Not a MAC address: %s\n", who);
            return 1;
        }
        if (!client_track_quarantine_clear(mac)) {
            printf("No room to record another pardon (limit %d). Use "
                   "\"quarantine off\" instead.\n", CLIENT_TRACK_MAX_CONFLICTS);
            return 1;
        }
        printf("Pardoned %s - it is being carried again, and will not be cut off "
               "again until a reboot.\n", who);
        return 0;
    }

    if (action != NULL) {
        printf("Unknown action \"%s\". Use: quarantine [clear <mac|all> | on | off]\n", action);
        return 1;
    }

    client_track_conflict_t conflicts[CLIENT_TRACK_MAX_CONFLICTS];
    int n = client_track_get_conflicts(conflicts, CLIENT_TRACK_MAX_CONFLICTS);

    uint32_t up = 0, down = 0;
    client_track_get_quarantine_drops(&up, &down);

    printf("Enforcement: %s\n",
           client_track_quarantine_enforcing()
               ? "on - a device on somebody else's address gets cut off"
               : "OFF - conflicts are reported but nothing is dropped");
    printf("Frames dropped: %u uplink, %u downlink\n", (unsigned)up, (unsigned)down);

    if (n == 0) {
        printf("No duplicate addresses.\n");
        return 0;
    }

    printf("\n%-15s %-18s %-18s %s\n", "ADDRESS", "CUT OFF", "KEPT BY", "STATE");
    for (int i = 0; i < n; i++) {
        client_track_conflict_t *c = &conflicts[i];
        char ip_str[16];
        esp_ip4addr_ntoa(&c->ip, ip_str, sizeof(ip_str));

        char blocked[18], holder[18];
        snprintf(blocked, sizeof(blocked), "%02x:%02x:%02x:%02x:%02x:%02x",
                 c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5]);
        snprintf(holder, sizeof(holder), "%02x:%02x:%02x:%02x:%02x:%02x",
                 c->peer_mac[0], c->peer_mac[1], c->peer_mac[2],
                 c->peer_mac[3], c->peer_mac[4], c->peer_mac[5]);

        char state[64];
        if (c->exempt) {
            strlcpy(state, "pardoned", sizeof(state));
        } else if (!c->armed) {
            strlcpy(state, "confirming", sizeof(state));
        } else if (!c->dropping) {
            strlcpy(state, "confirmed, not enforced", sizeof(state));
        } else {
            snprintf(state, sizeof(state), "dropping for %us", (unsigned)c->armed_s);
        }

        printf("%-15s %-18s %-18s %s\n", ip_str, blocked, holder, state);
    }
    return 0;
}

// Not a ban: the station is free to reassociate right away, so this is for a
// wedged client that isn't going to retry on its own, not for keeping a
// device off the network.
static int cmd_kick(int argc, char **argv)
{
    int errors = arg_parse(argc, argv, (void **)&kick_args);
    if (errors != 0) {
        arg_print_errors(stderr, kick_args.end, argv[0]);
        return 1;
    }

    const char *who = kick_args.mac->sval[0];
    uint8_t mac[6];
    if (!parse_mac(who, mac)) {
        printf("Not a MAC address: %s\n", who);
        return 1;
    }

    if (!client_track_kick_station(mac)) {
        printf("%s is not a connected WiFi client.\n", who);
        return 1;
    }

    printf("Kicked %s - it will need to reassociate.\n", who);
    return 0;
}

// ---- resets ----

// How long the boot that ended in this reset had been running. Not a clock
// reading and never presented as one - see reset_log.h.
//
// Whether this record's duration came off the flash checkpoint, which makes it
// a window rather than a reading. Both renderers below branch on it, so neither
// has to know how the other says it.
static bool reset_uptime_is_floor(const reset_log_entry_t *e)
{
    return !(e->flags & RESET_LOG_F_PREV_STATE) &&
            (e->flags & RESET_LOG_F_PREV_UPTIME_MIN) != 0;
}

// One unit, rounded down. The two-unit form below ("4h 12m") is right for an
// exact figure and wrong for a bound, where the second unit is precision the
// number does not have. The year is here so the last point on the checkpoint
// schedule reads "1y" rather than "365d".
static void fmt_short(uint32_t s, unsigned *val, const char **unit)
{
    if (s >= 365 * 86400) { *val = s / (365 * 86400); *unit = "y"; }
    else if (s >= 86400)  { *val = s / 86400;         *unit = "d"; }
    else if (s >= 3600)   { *val = s / 3600;          *unit = "h"; }
    else if (s >= 60)     { *val = s / 60;            *unit = "m"; }
    else                  { *val = s;                 *unit = "s"; }
}

// A checkpoint-recovered figure, marked as the bound it is: ">30s", ">5m",
// ">1y". The schedule is dense enough that printing the far end too would be
// noise - it stays available in the tooltip on the web page and in the JSON.
//
// "<" for the one record with no lower bound worth stating: a boot that wrote
// only its seed, where ">0s" would say nothing and the useful statement is the
// upper end.
static void fmt_bound(uint32_t floor_s, char *out, size_t n)
{
    unsigned v;
    const char *u;

    if (floor_s == 0) {
        fmt_short(reset_log_uptime_ceiling(0), &v, &u);
        snprintf(out, n, "<%u%s", v, u);
        return;
    }
    fmt_short(floor_s, &v, &u);
    snprintf(out, n, ">%u%s", v, u);
}

static void reset_ran_for(const reset_log_entry_t *e, char *out, size_t n)
{
    if (!(e->flags & (RESET_LOG_F_PREV_STATE | RESET_LOG_F_PREV_UPTIME_MIN))) {
        // No data at all: this record was written by firmware from before the
        // checkpoint existed, or that boot's one NVS write failed. Not the same
        // as a short boot, which now has a checkpoint of its own to say so.
        // Never substitute 0, which would be a claim.
        strlcpy(out, "unknown", n);
        return;
    }
    if (reset_uptime_is_floor(e)) {
        fmt_bound(e->prev_uptime_s, out, n);
        return;
    }
    uint32_t s = e->prev_uptime_s;
    if (s >= 86400) {
        snprintf(out, n, "%ud %uh", (unsigned)(s / 86400), (unsigned)((s % 86400) / 3600));
    } else if (s >= 3600) {
        snprintf(out, n, "%uh %um", (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
    } else if (s >= 60) {
        snprintf(out, n, "%um %us", (unsigned)(s / 60), (unsigned)(s % 60));
    } else {
        snprintf(out, n, "%us", (unsigned)s);
    }
}

// One sentence saying what happened, in the order the reader cares about rather
// than the order the fields are stored in. resets.html composes the same
// sentence from the same tokens - keep the two in step.
//
// Dispatches on the token rather than on the raw esp_reset_reason_t, so that
// reset_log_reason_name() stays the single place that decides what a code means
// and this file cannot come to a different conclusion than the JSON does. It
// also makes the branches line up one-for-one with the JavaScript, which only
// ever sees the tokens.
static void reset_what_happened(const reset_log_entry_t *e, char *out, size_t n)
{
    const char *reason = reset_log_reason_name(e->reason);

    if (e->flags & RESET_LOG_F_ROLLBACK) {
        strlcpy(out, "rolled back from a failed update", n);
    } else if (e->intent == RESET_INTENT_OTA) {
        strlcpy(out, "restart for firmware update", n);
    } else if (e->intent == RESET_INTENT_WIFI_SAVE) {
        strlcpy(out, "restart to apply WiFi settings", n);
    } else if (e->intent == RESET_INTENT_WAN_SAVE) {
        strlcpy(out, "restart to apply WAN settings", n);
    } else if (e->intent == RESET_INTENT_CONSOLE) {
        strlcpy(out, "console requested restart", n);
    } else if (e->intent == RESET_INTENT_FACTORY_RESET) {
        strlcpy(out, "factory reset", n);
    } else if (strcmp(reason, "panic") == 0) {
        strlcpy(out, "crashed (panic)", n);
    } else if (strcmp(reason, "int-wdt") == 0 || strcmp(reason, "task-wdt") == 0 ||
               strcmp(reason, "other-wdt") == 0) {
        strlcpy(out, "watchdog reset - something stopped responding", n);
    } else if (strcmp(reason, "brownout") == 0) {
        strlcpy(out, "brownout - the supply voltage dipped", n);
    } else if (strcmp(reason, "power-on") == 0) {
        // Never "power loss" on its own. A rail that collapses past the chip's
        // own reset threshold looks exactly like somebody flipping a switch, so
        // the record cannot tell them apart and should not pretend to.
        //
        // The rail witness splits this bucket where it can, and only here: every
        // other reason preserved RTC memory, which already proves the rail held,
        // so repeating it there would be noise. Held is reported as what was
        // measured - the rail stayed up, which is what an EN-pin reset does -
        // rather than as who did it, because a sag that reset the chip and
        // nothing else reads the same way.
        if (!(e->flags & RESET_LOG_F_RAIL_KNOWN)) {
            strlcpy(out, "power-on or power loss", n);
        } else if (e->flags & RESET_LOG_F_RAIL_HELD) {
            strlcpy(out, "reset without power cycle", n);
        } else {
            strlcpy(out, "cold boot after power loss", n);
        }
    } else if (strcmp(reason, "software") == 0) {
        // Nothing here restarts itself without tagging it first, so an untagged
        // software reset is a panic that got tidied up, or a path nobody
        // accounted for. Either way it is worth looking at.
        strlcpy(out, "restarted by software, untagged", n);
    } else {
        snprintf(out, n, "unknown reason (code %u)", (unsigned)e->reason);
    }

    // The boot-loop signature, and the whole reason reset_log_note_ready()
    // exists. Only claimed when the previous boot's state actually survived -
    // absent state is not evidence of a short boot.
    if ((e->flags & RESET_LOG_F_PREV_STATE) && !(e->flags & RESET_LOG_F_REACHED_READY)) {
        strlcat(out, ", during startup", n);
    }
}

// Shared with cmd_sysinfo so the two never describe the same boot differently.
static void describe_reset(const reset_log_entry_t *e, char *out, size_t n)
{
    char what[64], ran[24];
    reset_what_happened(e, what, sizeof(what));
    reset_ran_for(e, ran, sizeof(ran));

    // The boot number, and the date when that boot had a clock to know it by.
    // Appended rather than leading, and only when known, for the reason
    // resetText() gives on the dashboard: "why" is what this line is for and
    // "when" is the qualifier, and most devices will have no date at all.
    //
    // Built once because all four wordings below end with it. Written out per
    // wording it was the same parenthesis four times, and a fifth wording that
    // forgot the date is how this line and the dashboard's would drift into
    // describing one boot two ways - which is the drift the shared
    // describe_reset() exists to prevent.
    char tail[CLOCK_TIME_STR_MAX + 24];
    if (e->flags & RESET_LOG_F_BOOT_TIME) {
        char when[CLOCK_TIME_STR_MAX];
        clock_time_format((time_t)e->boot_epoch, when, sizeof(when));
        snprintf(tail, sizeof(tail), "(boot #%u, %s)", (unsigned)e->boot_seq, when);
    } else {
        snprintf(tail, sizeof(tail), "(boot #%u)", (unsigned)e->boot_seq);
    }

    if (e->flags & RESET_LOG_F_PREV_STATE) {
        snprintf(out, n, "%s after %s %s", what, ran, tail);
    } else if (reset_uptime_is_floor(e) && e->prev_uptime_s == 0) {
        // ran is already "<10s"; a sentence has the room to spell it out, and
        // "after <10s" reads worse than this does.
        snprintf(out, n, "%s in under %u seconds %s", what,
                 (unsigned)reset_log_uptime_ceiling(0), tail);
    } else if (reset_uptime_is_floor(e)) {
        // ran is ">5m"; again the mark belongs in the column, not the sentence.
        snprintf(out, n, "%s after more than %s %s", what, ran + 1, tail);
    } else {
        snprintf(out, n, "%s %s", what, tail);
    }
}

static struct {
    struct arg_str *tz;
    struct arg_lit *list;
    struct arg_end *end;
} time_args;

static int cmd_time(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&time_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, time_args.end, argv[0]);
        return 1;
    }

    if (time_args.list->count > 0) {
        const char *label, *tz;
        for (int i = 0; clock_cfg_zone(i, &label, &tz); i++) {
            printf("  %-24s %s\n", label, tz);
        }
        printf("\nAny other zone can be given as a POSIX TZ string directly.\n");
        return 0;
    }

    if (time_args.tz->count > 0) {
        clock_cfg_t cfg;
        clock_cfg_get(&cfg);

        // An IANA label from the shared list is accepted as well as a raw POSIX
        // string, because "America/Chicago" is what a person knows and
        // "CST6CDT,M3.2.0,M11.1.0" is what the C library needs. The lookup lives
        // in clock_cfg so /api/time accepts the same words; a NULL back means it
        // is not a label, which is the normal path for anyone who typed their own.
        const char *want = time_args.tz->sval[0];
        const char *resolved = clock_cfg_zone_from_label(want);
        strlcpy(cfg.tz, resolved ? resolved : want, sizeof(cfg.tz));

        const char *err_msg = NULL;
        if (!clock_cfg_validate(&cfg, &err_msg)) {
            printf("%s\n", err_msg);
            return 1;
        }
        if (clock_cfg_save(&cfg) != ESP_OK) {
            printf("Failed to save the timezone.\n");
            return 1;
        }
        printf("Timezone set to %s.\n", cfg.tz);
    }

    clock_cfg_t cfg;
    clock_cfg_get(&cfg);
    printf("timezone        %s\n", cfg.tz);

    time_t now;
    if (clock_time_now(&now)) {
        char now_str[CLOCK_TIME_STR_MAX];
        clock_time_format(now, now_str, sizeof(now_str));
        printf("time            %s %s\n", now_str, clock_time_zone_abbrev(now));

        const char *src = clock_time_source();
        if (strcmp(src, "carried") == 0) {
            printf("source          carried through RTC memory from an earlier boot - this\n"
                   "                reset did not cut power, so the clock survived it\n");
        } else {
            printf("source          NTP, over the WAN\n");
        }

        time_t last = clock_time_last_sync();
        if (last != 0) {
            char last_str[CLOCK_TIME_STR_MAX];
            clock_time_format(last, last_str, sizeof(last_str));
            printf("last sync       %s\n", last_str);
        }
        if (clock_time_stale()) {
            printf("\nNothing has corrected this clock in over a day. It runs on the internal\n"
                   "RC oscillator rather than a crystal, so by now it is likely to be out by\n"
                   "of the order of a minute. Treat the date as sound and the seconds as not.\n");
        }
    } else {
        printf("time            unknown\n");
        printf("\nThis device has no battery-backed clock, so it only knows the time while\n"
               "the WAN can reach an NTP server, and forgets it on the next power cut. The\n"
               "reset history is ordered and timed either way; see \"resets\".\n");
    }
    return 0;
}

static int cmd_resets(int argc, char **argv)
{
    reset_log_entry_t recs[RESET_LOG_MAX_RECORDS];
    int count = reset_log_get(recs, RESET_LOG_MAX_RECORDS);

    if (count == 0) {
        printf("No reset history yet.\n");
        return 0;
    }

    // One pass, three questions. The WHEN column and both footnotes below are
    // each gated on a property of the whole list, and asking each question in a
    // loop of its own was three walks over the same array to learn three things
    // one walk already knows.
    //
    // The WHEN column appears only when at least one record can fill it, which
    // on most devices is never - an empty column of dashes on every row would
    // cost 20 characters of an 80-column terminal to say nothing. Same
    // reasoning as the footnotes: explain, and occupy space, only when there is
    // something to explain.
    bool any_when = false, any_blank = false, any_floor = false;
    for (int i = 0; i < count; i++) {
        bool dated = (recs[i].flags & RESET_LOG_F_BOOT_TIME) != 0;
        any_when  = any_when  || dated;
        any_blank = any_blank || !dated;
        any_floor = any_floor || reset_uptime_is_floor(&recs[i]);
    }

    // The column is rendered into a cell that carries its own trailing space and
    // is empty when the column is off, so the two layouts share one format
    // string rather than having one each. Written out per layout it was four
    // format strings - header and row, present and absent - that had to be kept
    // in step by hand, and the one that drifts is the one nobody looks at
    // because it only appears on devices that have never had a clock.
    char when_hdr[CLOCK_TIME_STR_MAX + 1] = "";
    if (any_when) {
        snprintf(when_hdr, sizeof(when_hdr), "%-*s ", (int)(CLOCK_TIME_STR_MAX - 1), "WHEN");
    }
    printf("%-6s %-46s %s%9s  %s\n",
           "BOOT", "WHAT HAPPENED", when_hdr, "RAN FOR", "FIRMWARE");
    for (int i = 0; i < count; i++) {
        char seq_str[12];
        snprintf(seq_str, sizeof(seq_str), "#%u", (unsigned)recs[i].boot_seq);

        char what[64], ran[24];
        reset_what_happened(&recs[i], what, sizeof(what));
        reset_ran_for(&recs[i], ran, sizeof(ran));

        char fw[64];
        snprintf(fw, sizeof(fw), "%s %s%s", recs[i].version, recs[i].part,
                 (recs[i].flags & RESET_LOG_F_OTA_PENDING) ? " (on trial)" : "");

        char when[CLOCK_TIME_STR_MAX + 1] = "";
        if (any_when) {
            char t[CLOCK_TIME_STR_MAX] = "-";
            if (recs[i].flags & RESET_LOG_F_BOOT_TIME) {
                clock_time_format((time_t)recs[i].boot_epoch, t, sizeof(t));
            }
            snprintf(when, sizeof(when), "%-*s ", (int)(CLOCK_TIME_STR_MAX - 1), t);
        }
        printf("%-6s %-46s %s%9s  %s\n", seq_str, what, when, ran, fw);
    }

    // Only explained when there is something on screen to explain, so the
    // footer of a device that has never lost power stays two lines.
    if (any_floor) {
        printf("\nA marked figure (\">5m\", \"<10s\") is recovered from the uptime checkpoint\n"
               "in flash after a power event, and is a bound rather than a reading. An\n"
               "unmarked one came exactly from the counter in RTC memory.\n");
    }

    if (any_when) {
        // Named as the two different things a blank means, because they lead to
        // different conclusions: one is "this boot had no way to know" and the
        // other is "this record predates the field". Neither is a fault.
        if (any_blank) {
            printf("\nA \"-\" under WHEN is a boot that never had a clock: no WAN, or a WAN\n"
                   "that never reached an NTP server, or a record written before this\n"
                   "firmware. The order and the durations are unaffected.\n");
        }
    }

    printf("\n%d of %d records kept. Ordered first and dated only where the WAN could\n"
           "reach a time server - see \"time\". Kept across factory-reset on purpose.\n",
           count, RESET_LOG_MAX_RECORDS);
    return 0;
}

// ---- loglevel ----

static const struct {
    const char *name;
    esp_log_level_t level;
} log_levels[] = {
    {"none", ESP_LOG_NONE}, {"error", ESP_LOG_ERROR}, {"warn", ESP_LOG_WARN},
    {"info", ESP_LOG_INFO}, {"debug", ESP_LOG_DEBUG}, {"verbose", ESP_LOG_VERBOSE},
};

static struct {
    struct arg_str *level;
    struct arg_end *end;
} loglevel_args;

// Mirrors log_level_name() in web_server.c, down to the "unknown" fallback -
// the two tables are the same six names on purpose, so the console and the log
// page describe the device identically. Every esp_log_level_t value is in the
// table, so the fallback is unreachable defence.
static const char *level_name(esp_log_level_t level)
{
    for (size_t i = 0; i < sizeof(log_levels) / sizeof(log_levels[0]); i++) {
        if (log_levels[i].level == level) {
            return log_levels[i].name;
        }
    }
    return "unknown";
}

static int cmd_loglevel(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&loglevel_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, loglevel_args.end, argv[0]);
        return 1;
    }

    if (loglevel_args.level->count == 0) {
        // Both, because the capture level is what explains a serial level that
        // isn't delivering: nothing above it reaches the hook to be printed.
        printf("Serial log level: %s\n", level_name(log_buf_get_serial_level()));
        printf("Capture level:    %s\n", level_name(esp_log_level_get("*")));
        return 0;
    }

    const char *name = loglevel_args.level->sval[0];
    for (size_t i = 0; i < sizeof(log_levels) / sizeof(log_levels[0]); i++) {
        if (strcmp(name, log_levels[i].name) == 0) {
            log_buf_set_serial_level(log_levels[i].level);
            printf("Serial log level set to %s\n", name);

            // Only lines that pass the global filter reach the capture hook at
            // all, so asking for more serial verbosity than is being captured
            // is meaningless unless capture comes up with it. Raise it here
            // rather than printing a note and doing nothing: the web page can
            // set capture to "none", and this console has to be able to get
            // itself talking again without a reboot.
            //
            // Raise-only. Lowering capture to match would let someone at the
            // cable blind a web log page they can't see, and "quieter serial"
            // never needs it - that is what the serial level itself is for.
            esp_log_level_t captured = esp_log_level_get("*");
            if (log_levels[i].level > captured) {
                esp_log_level_set("*", log_levels[i].level);
                printf("Capture level raised to %s (was %s) so these lines can "
                       "reach this console.\n", name, level_name(captured));
            }

            // Both thresholds now say "debug", and the build emits no DEBUG
            // lines to pass them: ESP_LOGD and ESP_LOGV compile to nothing
            // above CONFIG_LOG_MAXIMUM_LEVEL. Without this the two confident
            // lines above are the only feedback for a change that cannot
            // produce a single extra line.
            if (log_levels[i].level > CONFIG_LOG_MAXIMUM_LEVEL) {
                printf("Note: this build compiles out everything above \"%s\" "
                       "(CONFIG_LOG_MAXIMUM_LEVEL), so no %s lines exist to "
                       "show.\n", level_name((esp_log_level_t)CONFIG_LOG_MAXIMUM_LEVEL),
                       name);
            }
            return 0;
        }
    }
    printf("Error: unknown level \"%s\" (want none|error|warn|info|debug|verbose)\n", name);
    return 1;
}

// ---- syslog ----

static struct {
    struct arg_str *state;
    struct arg_str *server;
    struct arg_int *port;
    struct arg_int *facility;
    struct arg_str *severity;
    struct arg_str *hostname;
    struct arg_end *end;
} syslog_args;

static void print_syslog_status(const syslog_cfg_t *cfg)
{
    syslog_status_t st;
    syslog_get_status(&st);

    char server[16];
    esp_ip4_addr_t addr = { .addr = cfg->server_ip };
    esp_ip4addr_ntoa(&addr, server, sizeof(server));

    printf("Syslog:     %s\n", cfg->enabled ? "enabled" : "disabled");
    if (cfg->server_ip == 0) {
        printf("Collector:  not set\n");
    } else {
        printf("Collector:  %s:%u   (facility %u %s, minimum severity %s)\n",
               server, (unsigned)cfg->port, (unsigned)cfg->facility,
               syslog_cfg_facility_name(cfg->facility),
               syslog_cfg_severity_name(cfg->min_severity));
    }
    printf("Hostname:   %s\n", cfg->hostname);
    printf("Socket:     %s\n", st.bound ? "bound to the bridge"
                                        : "not bound - waiting for the bridge network");
    printf("Sent:       %u    Waiting: %u    Filtered: %u    Aged out: %u    Failed: %u (last errno %d)\n",
           (unsigned)st.sent, (unsigned)st.backlog, (unsigned)st.filtered,
           (unsigned)st.aged_out, (unsigned)st.send_fail, st.last_errno);
    printf("\n"
           "Nothing here can tell whether the collector received any of it - UDP has\n"
           "no acknowledgement, and this device has no route to anywhere that could\n"
           "say. \"Sent\" means handed to the network.\n");
}

static int cmd_syslog(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&syslog_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, syslog_args.end, argv[0]);
        return 1;
    }

    syslog_cfg_t cfg;
    syslog_cfg_get(&cfg);

    bool any_given = syslog_args.state->count > 0 || syslog_args.server->count > 0 ||
                     syslog_args.port->count > 0 || syslog_args.facility->count > 0 ||
                     syslog_args.severity->count > 0 || syslog_args.hostname->count > 0;
    if (!any_given) {
        print_syslog_status(&cfg);
        return 0;
    }

    // Any field not explicitly given keeps its current saved value, the same
    // contract "wifi" and the web form follow.
    if (syslog_args.state->count > 0) {
        const char *state = syslog_args.state->sval[0];
        if (strcmp(state, "on") == 0) {
            cfg.enabled = true;
        } else if (strcmp(state, "off") == 0) {
            cfg.enabled = false;
        } else {
            printf("Error: expected \"on\" or \"off\", got \"%s\"\n", state);
            return 1;
        }
    }
    if (syslog_args.server->count > 0) {
        esp_ip4_addr_t addr;
        addr.addr = esp_ip4addr_aton(syslog_args.server->sval[0]);
        if (addr.addr == 0) {
            printf("Error: \"%s\" is not an IPv4 address\n", syslog_args.server->sval[0]);
            return 1;
        }
        cfg.server_ip = addr.addr;
    }
    if (syslog_args.port->count > 0) {
        int port = syslog_args.port->ival[0];
        if (port < 1 || port > 65535) {
            printf("Error: port must be 1-65535 (514 is the syslog default)\n");
            return 1;
        }
        cfg.port = (uint16_t)port;
    }
    if (syslog_args.facility->count > 0) {
        int facility = syslog_args.facility->ival[0];
        if (facility < 0 || facility > 23) {
            printf("Error: facility must be 0-23 (23 is local7)\n");
            return 1;
        }
        cfg.facility = (uint8_t)facility;
    }
    if (syslog_args.severity->count > 0) {
        uint8_t severity;
        if (!syslog_cfg_severity_from_name(syslog_args.severity->sval[0], &severity)) {
            printf("Error: unknown severity \"%s\" (want emergency|alert|critical|error|"
                   "warning|notice|info|debug)\n", syslog_args.severity->sval[0]);
            return 1;
        }
        cfg.min_severity = severity;
    }
    if (syslog_args.hostname->count > 0) {
        strlcpy(cfg.hostname, syslog_args.hostname->sval[0], sizeof(cfg.hostname));
        // An explicitly blank name means "go back to the default", resolved by
        // syslog_cfg_save()/_get() rather than here.
        if (cfg.hostname[0] == '\0') {
            syslog_cfg_default_hostname(cfg.hostname, sizeof(cfg.hostname));
        }
    }

    uint32_t subnet_ip = 0, subnet_mask = 0;
    syslog_get_subnet(&subnet_ip, &subnet_mask);

    const char *err_msg = NULL;
    if (!syslog_cfg_validate(&cfg, subnet_ip, subnet_mask, &err_msg)) {
        printf("Error: %s\n", err_msg);
        return 1;
    }

    if (syslog_cfg_save(&cfg) != ESP_OK) {
        printf("Error: failed to save syslog config\n");
        return 1;
    }
    syslog_config_changed();

    printf("Saved. In effect now; no reboot needed.\n\n");
    print_syslog_status(&cfg);
    return 0;
}

// ---- wan ----

static struct {
    struct arg_str *state;
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_str *ports;
    struct arg_end *end;
} wan_args;

static void print_wan_status(const wan_cfg_t *cfg)
{
    wan_status_t st;
    wan_get_status(&st);

    char ports[WAN_CFG_PORTS_STR_MAX];
    wan_cfg_format_ports(cfg, ports, sizeof(ports));

    printf("WAN: %s\n", cfg->enabled ? "on" : "off");
    printf("  Upstream network: %s\n", cfg->ssid[0] ? cfg->ssid : "(not set)");
    printf("  Password:         %s\n", cfg->password[0] ? "(set)" : "(open network)");
    printf("  Allowed ports:    %s\n", ports[0] ? ports : "(none)");
    printf("  State:            %s\n", wan_state_name(st.state));

    if (st.state == WAN_STATE_SUBNET_CONFLICT) {
        printf("  The upstream network hands out addresses on 192.168.5.x, the same range\n"
               "  this bridge uses. Change the upstream router's range, or the WAN\n"
               "  cannot work.\n");
    }
    if (st.ip.addr != 0) {
        printf("  Address:          " IPSTR "/" IPSTR "\n", IP2STR(&st.ip), IP2STR(&st.netmask));
        printf("  Gateway:          " IPSTR "\n", IP2STR(&st.gw));
        printf("  DNS:              " IPSTR "\n", IP2STR(&st.dns));
        printf("  Signal:           %d dBm\n", st.rssi);
    }
    if (st.ap_channel != 0) {
        printf("  Radio channel:    %u", (unsigned)st.ap_channel);
        if (st.ap_channel_configured != 0 && st.ap_channel != st.ap_channel_configured) {
            printf(" (the \"wifi\" setting asks for %u, but one radio cannot serve two "
                   "channels - the upstream network's wins)", (unsigned)st.ap_channel_configured);
        }
        printf("\n");
    }
    if (st.retry_in_s != 0) {
        printf("  Retrying in:      %u s\n", (unsigned)st.retry_in_s);
    }
    printf("  NAT:              %s\n", st.napt_on ? "enabled" : "not enabled");
    printf("  Connects/disconnects: %u/%u (last reason %u)\n",
           (unsigned)st.connects, (unsigned)st.disconnects, (unsigned)st.last_reason);
    printf("  Out: %u allowed, %u blocked by the port list (DNS and NTP bypass it)\n",
           (unsigned)st.tx_allowed, (unsigned)st.tx_blocked);
    printf("  In:  %u allowed, %u blocked\n",
           (unsigned)st.rx_allowed, (unsigned)st.rx_blocked);
}

static int cmd_wan(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&wan_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wan_args.end, argv[0]);
        return 1;
    }

    wan_cfg_t cfg;
    wan_cfg_get(&cfg);
    const wan_cfg_t before = cfg;

    bool any_given = wan_args.state->count > 0 || wan_args.ssid->count > 0 ||
                     wan_args.password->count > 0 || wan_args.ports->count > 0;
    if (!any_given) {
        print_wan_status(&cfg);
        return 0;
    }

    // Any field not explicitly given keeps its current saved value, the same
    // contract "wifi", "syslog" and the web form follow.
    if (wan_args.state->count > 0) {
        const char *state = wan_args.state->sval[0];
        if (strcmp(state, "on") == 0) {
            cfg.enabled = true;
        } else if (strcmp(state, "off") == 0) {
            cfg.enabled = false;
        } else {
            printf("Error: expected \"on\" or \"off\", got \"%s\"\n", state);
            return 1;
        }
    }
    if (wan_args.ssid->count > 0) {
        strlcpy(cfg.ssid, wan_args.ssid->sval[0], sizeof(cfg.ssid));
    }
    if (wan_args.password->count > 0) {
        strlcpy(cfg.password, wan_args.password->sval[0], sizeof(cfg.password));
    }
    if (wan_args.ports->count > 0) {
        const char *err_msg = NULL;
        if (!wan_cfg_parse_ports(wan_args.ports->sval[0], cfg.ports, &cfg.nports, &err_msg)) {
            printf("Error: %s\n", err_msg);
            return 1;
        }
    }

    const char *err_msg = NULL;
    if (!wan_cfg_validate(&cfg, &err_msg)) {
        printf("Error: %s\n", err_msg);
        return 1;
    }

    // Only the radio's half of the configuration needs a restart; the allowlist
    // is read from a published snapshot on every packet. See wan_post_handler().
    bool radio_changed = (cfg.enabled != before.enabled) ||
                         strcmp(cfg.ssid, before.ssid) != 0 ||
                         strcmp(cfg.password, before.password) != 0;

    if (wan_cfg_save(&cfg) != ESP_OK) {
        printf("Error: failed to save uplink config\n");
        return 1;
    }

    if (!radio_changed) {
        wan_ports_changed();
        printf("Saved. In effect now; no reboot needed.\n\n");
        print_wan_status(&cfg);
        return 0;
    }

    printf("Saved. Restarting to apply - every WiFi client will disconnect and rejoin,\n"
           "on the upstream network's channel if the WAN comes up.\n");
    reset_log_note_intent(RESET_INTENT_WAN_SAVE);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return 0;
}

// ---- reboot ----

static int cmd_reboot(int argc, char **argv)
{
    printf("Rebooting...\n");
    fflush(stdout);
    reset_log_note_intent(RESET_INTENT_CONSOLE);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

// ---- factory-reset ----

static esp_err_t erase_nvs_namespace(const char *namespace)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(namespace, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(nvs_handle);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return err;
}

static struct {
    struct arg_str *confirm;
    struct arg_end *end;
} factory_reset_args;

static int cmd_factory_reset(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&factory_reset_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, factory_reset_args.end, argv[0]);
        return 1;
    }

    if (factory_reset_args.confirm->count == 0 || strcmp(factory_reset_args.confirm->sval[0], "yes") != 0) {
        printf("This erases the saved WiFi and admin credentials, the saved DHCP\n"
               "address reservations, the remote syslog settings, and the internet\n"
               "uplink settings, restoring compiled-in defaults, then reboots.\n"
               "Clients will be given fresh addresses, the device will stop shipping\n"
               "its log anywhere, and it will stop joining any upstream network.\n"
               "\n"
               "The reboot/crash history is kept. It is evidence about the device\n"
               "rather than configuration of it, and a factory reset is often the\n"
               "first thing tried on a device that keeps restarting - erasing it\n"
               "would destroy the record of the very fault being chased. Use\n"
               "\"resets\" to read it.\n"
               "\n"
               "The timezone is kept too, because that history is: a reset does not\n"
               "move the device, and clearing the zone would leave the one thing\n"
               "deliberately preserved rendering its dates in the wrong one. Use\n"
               "\"time\" to change it.\n"
               "\n"
               "Run \"factory-reset yes\" to confirm.\n");
        return 0;
    }

    // The syslog settings are erased along with the rest, deliberately: a device
    // sent away for repair or handed on should not carry on shipping its log to
    // somebody's old collector. The WAN settings go for the stronger version
    // of the same reason - they contain another network's WiFi password.
    //
    // clock_cfg is the one configuration namespace deliberately NOT erased, and
    // it is kept for the same reason the reset history is. That history survives
    // a factory reset because it is evidence rather than configuration; its
    // dates are rendered through this zone; so erasing the zone would leave the
    // one thing this command promises to preserve displaying its dates hours
    // out. A timezone is also not a secret and not stale on a device that
    // changed hands - the box did not move.
    esp_err_t err1 = erase_nvs_namespace("wifi_config");
    esp_err_t err2 = erase_nvs_namespace("auth_cfg");
    esp_err_t err3 = erase_nvs_namespace("dhcp_leases");
    esp_err_t err4 = erase_nvs_namespace("syslog_cfg");
    esp_err_t err5 = erase_nvs_namespace("wan_cfg");
    if (err1 != ESP_OK || err2 != ESP_OK || err3 != ESP_OK || err4 != ESP_OK || err5 != ESP_OK) {
        printf("Error: failed to erase config (wifi: %s, admin: %s, leases: %s, syslog: %s, "
               "uplink: %s)\n",
               esp_err_to_name(err1), esp_err_to_name(err2), esp_err_to_name(err3),
               esp_err_to_name(err4), esp_err_to_name(err5));
        return 1;
    }

    printf("Config erased. Rebooting...\n");
    fflush(stdout);
    reset_log_note_intent(RESET_INTENT_FACTORY_RESET);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

// ---- registration ----

static void register_commands(void)
{
    wifi_args.ssid = arg_str0("s", "ssid", "<ssid>", "new SSID (1-31 chars)");
    wifi_args.password = arg_str0("p", "password", "<password>", "new password (8-63 chars); blank keeps current");
    wifi_args.channel = arg_int0("c", "channel", "<1|6|11>", "WiFi channel; omitted keeps current");
    wifi_args.end = arg_end(3);
    const esp_console_cmd_t wifi_cmd = {
        .command = "wifi",
        .help = "Show or set the WiFi AP SSID/password/channel. No args shows the current config. "
                "Changing any of -s/-p/-c reboots the device to apply.",
        .hint = NULL,
        .func = &cmd_wifi,
        .argtable = &wifi_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_cmd));

    admin_args.user = arg_str0("u", "user", "<username>", "new admin username (1-31 chars)");
    admin_args.password = arg_str0("p", "password", "<password>", "new admin password (4-63 chars)");
    admin_args.end = arg_end(2);
    const esp_console_cmd_t admin_cmd = {
        .command = "admin",
        .help = "Show or change the web UI admin username/password. No args shows the current username.",
        .hint = NULL,
        .func = &cmd_admin,
        .argtable = &admin_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&admin_cmd));

    const esp_console_cmd_t sysinfo_cmd = {
        .command = "sysinfo",
        .help = "Show uptime, heap, CPU load, and network traffic stats.",
        .hint = NULL,
        .func = &cmd_sysinfo,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&sysinfo_cmd));

    const esp_console_cmd_t clients_cmd = {
        .command = "clients",
        .help = "List currently active bridge clients (WiFi + Ethernet).",
        .hint = NULL,
        .func = &cmd_clients,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&clients_cmd));

    const esp_console_cmd_t leases_cmd = {
        .command = "leases",
        .help = "List DHCP leases and the MAC->IP reservations kept in flash across reboots.",
        .hint = NULL,
        .func = &cmd_leases,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&leases_cmd));

    quarantine_args.action = arg_str0(NULL, NULL, "<clear|on|off>",
                                       "clear pardons a device; on/off switch the dropping");
    quarantine_args.mac = arg_str0(NULL, NULL, "<mac|all>", "which device to pardon");
    quarantine_args.end = arg_end(2);
    const esp_console_cmd_t quarantine_cmd = {
        .command = "quarantine",
        .help = "Show devices cut off for using an address that was already somebody else's. "
                "No args lists them. \"quarantine clear <mac|all>\" puts a device back on the "
                "network and stops it being cut off again until a reboot. \"quarantine off\" "
                "stops dropping altogether while still detecting and reporting conflicts.",
        .hint = NULL,
        .func = &cmd_quarantine,
        .argtable = &quarantine_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&quarantine_cmd));

    kick_args.mac = arg_str1(NULL, NULL, "<mac>", "WiFi station to disconnect");
    kick_args.end = arg_end(1);
    const esp_console_cmd_t kick_cmd = {
        .command = "kick",
        .help = "Tear down a WiFi station's current association so it has to reassociate. "
                "Not a ban - a station that retries on its own is free to reconnect "
                "immediately. For a client that's wedged and won't retry itself, without "
                "rebooting the whole bridge.",
        .hint = NULL,
        .func = &cmd_kick,
        .argtable = &kick_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&kick_cmd));

    time_args.tz = arg_str0(NULL, NULL, "<zone>", "IANA name or POSIX TZ string, e.g. America/Chicago");
    time_args.list = arg_lit0("l", "list", "list the built-in zones");
    time_args.end = arg_end(2);
    const esp_console_cmd_t time_cmd = {
        .command = "time",
        .help = "Show the clock and the timezone it is rendered in, or set the timezone. "
                "The clock itself comes from NTP over the WAN and cannot be set by hand.",
        .hint = NULL,
        .func = &cmd_time,
        .argtable = &time_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&time_cmd));

    const esp_console_cmd_t resets_cmd = {
        .command = "resets",
        .help = "List why this device restarted, most recent first. Kept in flash across "
                "reboots, and deliberately across factory-reset.",
        .hint = NULL,
        .func = &cmd_resets,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&resets_cmd));

    loglevel_args.level = arg_str0(NULL, NULL, "<level>", "none|error|warn|info|debug|verbose");
    loglevel_args.end = arg_end(1);
    const esp_console_cmd_t loglevel_cmd = {
        .command = "loglevel",
        .help = "Show or set how much log output reaches this serial console (defaults to \"warn\" so it doesn't bury command output). Raises the web log page's capture level too if that is what's holding the output back.",
        .hint = NULL,
        .func = &cmd_loglevel,
        .argtable = &loglevel_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&loglevel_cmd));

    syslog_args.state = arg_str0(NULL, NULL, "<on|off>", "turn log shipping on or off");
    syslog_args.server = arg_str0("s", "server", "<ip>", "collector address, on 192.168.5.0/24");
    syslog_args.port = arg_int0("p", "port", "<1-65535>", "UDP port; 514 is the syslog default");
    syslog_args.facility = arg_int0("f", "facility", "<0-23>", "syslog facility; 23 is local7");
    syslog_args.severity = arg_str0("l", "level", "<severity>", "lowest severity to send");
    syslog_args.hostname = arg_str0("n", "hostname", "<name>", "HOSTNAME field; blank restores the MAC-derived default");
    syslog_args.end = arg_end(6);
    const esp_console_cmd_t syslog_cmd = {
        .command = "syslog",
        .help = "Show or set the remote syslog client, which ships the device log to a "
                "collector as RFC 5424 datagrams. No args shows the settings and counters. "
                "The collector must be on this bridge's own subnet - it has no gateway. "
                "Takes effect immediately: unlike \"wifi\", nothing here needs a reboot.",
        .hint = NULL,
        .func = &cmd_syslog,
        .argtable = &syslog_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&syslog_cmd));

    wan_args.state = arg_str0(NULL, NULL, "<on|off>", "turn the WAN on or off");
    wan_args.ssid = arg_str0("s", "ssid", "<ssid>", "upstream network to join");
    wan_args.password = arg_str0("p", "password", "<pass>", "upstream password; omit for an open network");
    wan_args.ports = arg_str0(NULL, "ports", "<list>", "allowed destination ports, e.g. 2101,21116/udp");
    wan_args.end = arg_end(5);
    const esp_console_cmd_t wan_cmd = {
        .command = "wan",
        .help = "Show or set WiFi as WAN, which routes LAN clients out through "
                "another WiFi network with NAT. No args shows the state and counters. Only the "
                "listed ports can be reached through it (2101 is the usual NTRIP caster port; "
                "the web UI keeps that list on its own Port Whitelist page); "
                "everything else, in both directions, is dropped, except DNS to the resolver the "
                "upstream network hands out and NTP on 123/udp, which are always allowed and do "
                "not belong on the list. An entry is a port number, "
                "optionally suffixed /tcp or /udp; bare means TCP. Changing --ports takes "
                "effect immediately; changing the network, password or on/off reboots. Note the "
                "device has one radio: while the WAN is up, this bridge's own WiFi is forced "
                "onto the upstream network's channel.",
        .hint = NULL,
        .func = &cmd_wan,
        .argtable = &wan_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wan_cmd));

    const esp_console_cmd_t reboot_cmd = {
        .command = "reboot",
        .help = "Restart the device.",
        .hint = NULL,
        .func = &cmd_reboot,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reboot_cmd));

    factory_reset_args.confirm = arg_str0(NULL, NULL, "<yes>", "pass \"yes\" to actually erase and reboot");
    factory_reset_args.end = arg_end(1);
    const esp_console_cmd_t factory_reset_cmd = {
        .command = "factory-reset",
        .help = "Erase saved WiFi/admin config, DHCP reservations, syslog settings and WAN settings back to compiled-in defaults and reboot. The reboot/crash history is deliberately kept.",
        .hint = NULL,
        .func = &cmd_factory_reset,
        .argtable = &factory_reset_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&factory_reset_cmd));
}

esp_err_t serial_console_init(void)
{
    // Routine INFO-level traffic/connect logs would otherwise interleave with
    // command output on this same UART, so serial stays at "warn" and above.
    // That used to be done by pinning the *global* level, which also starved
    // the web log page of anything to show - the two thresholds are now
    // separate (see log_buf.h): everything from INFO up is captured for the
    // web page, and only warnings and errors reach this UART. "loglevel"
    // raises the serial side back for debugging, and raises the capture level
    // with it if that is what's in the way.
    esp_log_level_set("*", ESP_LOG_INFO);
    log_buf_set_serial_level(ESP_LOG_WARN);

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "aog-bridge>";

    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_uart(&uart_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_new_repl_uart failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_console_register_help_command());
    register_commands();

    return esp_console_start_repl(repl);
}
