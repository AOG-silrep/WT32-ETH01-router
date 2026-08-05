#include <string.h>
#include <stdio.h>
#include "serial_console.h"
#include "wifi_cfg.h"
#include "auth_cfg.h"
#include "client_track.h"
#include "dhcp_server.h"
#include "sys_monitor.h"
#include "eth_link.h"
#include "log_buf.h"
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
    printf("Eth link:       %u flap%s, %u s in this state\n",
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

        char state[48];
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

// ---- reboot ----

static int cmd_reboot(int argc, char **argv)
{
    printf("Rebooting...\n");
    fflush(stdout);
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
        printf("This erases the saved WiFi and admin credentials and the saved DHCP\n"
               "address reservations, restoring compiled-in defaults, then reboots.\n"
               "Clients will be given fresh addresses. Run \"factory-reset yes\" to confirm.\n");
        return 0;
    }

    esp_err_t err1 = erase_nvs_namespace("wifi_config");
    esp_err_t err2 = erase_nvs_namespace("auth_cfg");
    esp_err_t err3 = erase_nvs_namespace("dhcp_leases");
    if (err1 != ESP_OK || err2 != ESP_OK || err3 != ESP_OK) {
        printf("Error: failed to erase config (wifi: %s, admin: %s, leases: %s)\n",
               esp_err_to_name(err1), esp_err_to_name(err2), esp_err_to_name(err3));
        return 1;
    }

    printf("Config erased. Rebooting...\n");
    fflush(stdout);
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
        .help = "Erase saved WiFi/admin config and DHCP reservations back to compiled-in defaults and reboot.",
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
