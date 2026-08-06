#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_eth_phy_lan87xx.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_br_glue.h"
#include "driver/gpio.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/ip_addr.h"
#include "netif/ethernet.h"
#include <sys/socket.h>
#include "wifi_cfg.h"
#include "dhcp_server.h"
#include "client_track.h"
#include "sys_monitor.h"
#include "eth_link.h"
#include "reset_log.h"
#include "rail_witness.h"
#include "auth_cfg.h"
#include "web_server.h"
#include "serial_console.h"
#include "log_buf.h"

static const char *TAG = "AOG-BRIDGE";

// Network Configuration
#define BRIDGE_IP      "192.168.5.1"
#define BRIDGE_NETMASK "255.255.255.0"
#define BRIDGE_GW      "192.168.5.1"
// A range per bridge port, decided by the port each DHCP request arrives on.
// The wired one is small because the Ethernet port is normally one machine, and
// the WiFi one stops well short of the end of the subnet to leave .102 - .254
// clear for the statically-addressed AgOpenGPS modules that live up there.
#define ETH_DHCP_START  "192.168.5.2"
#define ETH_DHCP_END    "192.168.5.9"
#define WIFI_DHCP_START "192.168.5.10"
#define WIFI_DHCP_END   "192.168.5.101"

// ETH Configuration
#define ETH_PHY_ADDR        1
#define ETH_PHY_POWER_PIN   16
#define ETH_MDC_GPIO        23
#define ETH_MDIO_GPIO       18

static void wifi_init_softap(esp_netif_t **out_wifi_netif)
{
    char ssid[WIFI_CFG_SSID_MAX_LEN];
    char password[WIFI_CFG_PASSWORD_MAX_LEN];
    uint8_t channel;

    wifi_cfg_load(ssid, password, &channel);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_LOGI(TAG, "Initializing WiFi Access Point...");

    // Create the WiFi AP netif as a bridge port: no IP/DHCP of its own,
    // the bridge netif owns the single IP and DHCP server for the LAN.
    esp_netif_inherent_config_t netif_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
    netif_cfg.flags = ESP_NETIF_FLAG_AUTOUP;
    netif_cfg.ip_info = NULL;

    esp_netif_t *wifi_netif = esp_netif_create_wifi(WIFI_IF_AP, &netif_cfg);
    assert(wifi_netif);
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_ap_handlers());

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(wifi_cfg_apply(ssid, password, channel));

    ESP_LOGI(TAG, "WiFi AP configured");
    ESP_LOGI(TAG, "  SSID: %s", ssid);
    ESP_LOGI(TAG, "  Channel: %u", channel);

    *out_wifi_netif = wifi_netif;
}

static void eth_init(esp_netif_t **out_eth_netif, esp_eth_handle_t *out_eth_handle) {
    ESP_LOGI(TAG, "Initializing Ethernet...");

    // Power up PHY
    gpio_set_direction(ETH_PHY_POWER_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(ETH_PHY_POWER_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(500));

    // MAC and PHY configuration

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = -1;

    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.smi_gpio.mdc_num = ETH_MDC_GPIO;
    esp32_emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;

    ESP_LOGI(TAG, "Setting MAC config...");

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;

    ESP_LOGI(TAG, "Installing Ethernet driver...");

    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    // Create the Ethernet netif as a bridge port: flags must be zero so it
    // takes no IP/DHCP of its own, and attach the driver to it.
    esp_netif_inherent_config_t eth_netif_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    eth_netif_cfg.if_key = "ETH_0";
    eth_netif_cfg.if_desc = "eth";
    eth_netif_cfg.route_prio = 50;
    eth_netif_cfg.flags = 0;
    esp_netif_config_t eth_netif_config = {
        .base = &eth_netif_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    esp_netif_t *eth_netif = esp_netif_new(&eth_netif_config);
    assert(eth_netif);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    ESP_LOGI(TAG, "Ethernet initialization complete");

    *out_eth_netif = eth_netif;
    *out_eth_handle = eth_handle;
}

static void setup_bridge(esp_netif_t *eth_netif, esp_netif_t *wifi_netif, const uint8_t *common_mac,
                          esp_netif_t **out_br_netif)
{
    ESP_LOGI(TAG, "Configuring bridge + DHCP server...");

    esp_netif_ip_info_t br_ip_info;
    memset(&br_ip_info, 0, sizeof(br_ip_info));
    inet_pton(AF_INET, BRIDGE_IP, &br_ip_info.ip);
    inet_pton(AF_INET, BRIDGE_NETMASK, &br_ip_info.netmask);
    inet_pton(AF_INET, BRIDGE_GW, &br_ip_info.gw);

    // max_fdb_dyn_entries is a hard cap on how many MAC addresses the bridge
    // can remember a port for, and running out of it is silent and costly.
    // lwIP's bridgeif_fdb_update_src() simply stops learning when the table is
    // full - its last line is literally "not found, no free entry -> flood" -
    // and bridgeif_fdb_get_dst_ports() then answers BR_FLOOD for every address
    // it never learnt. A flooded frame goes out *both* ports, so each unicast
    // to an unlearnt client is also transmitted over the air to no one, which
    // costs airtime on the side of the bridge that has least to spare.
    // Entries are only reclaimed on a 5-minute timeout (BR_FDB_TIMEOUT_SEC),
    // so a table that fills once stays full.
    //
    // 32 rather than 10 to keep clear of CLIENT_TRACK_MAX_CLIENTS (16), which
    // is what this device claims it can track: the forwarding table should not
    // be the first thing to give out. Each entry is a MAC, a port index and a
    // timestamp, so the whole increase costs a few hundred bytes.
    bridgeif_config_t bridge_config = {
        .max_fdb_dyn_entries = 32,
        .max_fdb_sta_entries = 2,
        .max_ports = 2,
    };

    esp_netif_inherent_config_t br_netif_cfg = ESP_NETIF_INHERENT_DEFAULT_BR_DHCPS();
    // Drop ESP_NETIF_DHCP_SERVER from the inherent defaults, the same way the
    // two bridge ports above override their own flags. dhcp_server.c owns port
    // 67 on this LAN, and everything about IDF's server is gated on this one
    // bit - esp_netif never calls dhcps_new() or dhcps_start() without it - so
    // clearing it means no second server is ever created, rather than one
    // being started and stopped again. Nothing else about the netif changes:
    // it still comes up, still becomes the default route, and still takes its
    // static address from ip_info below.
    br_netif_cfg.flags = ESP_NETIF_FLAG_IS_BRIDGE;
    br_netif_cfg.ip_info = &br_ip_info;
    br_netif_cfg.bridge_info = &bridge_config;
    memcpy(br_netif_cfg.mac, common_mac, sizeof(br_netif_cfg.mac));

    esp_netif_config_t br_netif_config = {
        .base = &br_netif_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_BR,
    };
    esp_netif_t *br_netif = esp_netif_new(&br_netif_config);
    if (br_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create bridge netif!");
        return;
    }

    esp_netif_br_glue_handle_t br_glue = esp_netif_br_glue_new();
    ESP_ERROR_CHECK(esp_netif_br_glue_add_port(br_glue, eth_netif));
    ESP_ERROR_CHECK(esp_netif_br_glue_add_wifi_port(br_glue, wifi_netif));
    // Attaching only wires up the glue's port event handlers - it does not
    // start the bridge netif. That happens when esp_eth_start()/esp_wifi_start()
    // below raise ETHERNET_EVENT_START/WIFI_EVENT_AP_START, and the netif comes
    // up on the first link-up or station association after that. The DHCP
    // server below needs none of it to have happened yet.
    ESP_ERROR_CHECK(esp_netif_attach(br_netif, br_glue));

    ESP_LOGI(TAG, "Bridge started");

    esp_ip4_addr_t pool_start, pool_end, wired_start, wired_end;
    inet_pton(AF_INET, WIFI_DHCP_START, &pool_start);
    inet_pton(AF_INET, WIFI_DHCP_END, &pool_end);
    inet_pton(AF_INET, ETH_DHCP_START, &wired_start);
    inet_pton(AF_INET, ETH_DHCP_END, &wired_end);
    ESP_ERROR_CHECK(dhcp_server_start(br_netif, br_ip_info.ip, br_ip_info.netmask,
                                       pool_start, pool_end, wired_start, wired_end));

    *out_br_netif = br_netif;
}

void app_main(void)
{
    // First thing in app_main so the banner below and every subsystem's
    // startup logging land in the ring the web log page reads. Anything
    // earlier (bootloader, pre-scheduler ROM output) goes to the UART only.
    log_buf_init();

    ESP_LOGI(TAG, "\n====================================");
    ESP_LOGI(TAG, "WT32-ETH01 Ethernet-WiFi Bridge");
    ESP_LOGI(TAG, "Shared Network: 192.168.5.0/24");
    ESP_LOGI(TAG, "====================================\n");
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND){
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Why this early: the record has to be in flash before this boot can produce
    // a reset of its own. A boot that dies in eth_init() below must not be the
    // one that loses the evidence of the last five times it did the same thing,
    // which is the case this module exists for. It needs NVS, just above, and
    // nothing else - esp_reset_reason() was latched by the startup code long
    // before app_main, and the OTA state it reads comes straight off flash.
    //
    // Not ESP_ERROR_CHECK'd, unlike every other init in this function. Those are
    // load-bearing for bridging; this one is evidence. Refusing to boot because
    // the crash history could not be written would be a self-inflicted brick on
    // a device installed in a field.
    //
    // The one thing allowed in front of it is the rail witness, and only because
    // it is an input to the record rather than a consumer of it - the PHY
    // register it reads is cleared by eth_init() a few lines below, so there is
    // no later point at which the reading still exists. That is also why
    // rail_witness_probe() is written to be incapable of aborting: it allocates
    // nothing, bounds every loop, and returns UNKNOWN for anything it cannot
    // establish.
    const rail_witness_config_t rail_cfg = {
        .mdc_gpio   = ETH_MDC_GPIO,
        .mdio_gpio  = ETH_MDIO_GPIO,
        .power_gpio = ETH_PHY_POWER_PIN,
        .phy_addr   = ETH_PHY_ADDR,
    };
    rail_witness_t rail = rail_witness_probe(&rail_cfg);

    esp_err_t reset_log_ret = reset_log_init(rail == RAIL_WITNESS_HELD    ? RESET_RAIL_HELD :
                                             rail == RAIL_WITNESS_DROPPED ? RESET_RAIL_DROPPED :
                                                                            RESET_RAIL_UNKNOWN);
    if (reset_log_ret != ESP_OK) {
        ESP_LOGW(TAG, "reset history unavailable: %s", esp_err_to_name(reset_log_ret));
    }

    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Single shared MAC address for the bridge (Ethernet + WiFi AP present as one device)
    uint8_t common_mac[6];
    ESP_ERROR_CHECK(esp_read_mac(common_mac, ESP_MAC_ETH));

    // Initialize Ethernet
    esp_netif_t *eth_netif;
    esp_eth_handle_t eth_handle;
    eth_init(&eth_netif, &eth_handle);
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, common_mac));

    // Ethernet port state (link, speed, duplex) for the web UI and console.
    // Has to be registered before esp_eth_start() below, or the first
    // link-up is missed and the port reads as down until the cable moves.
    ESP_ERROR_CHECK(eth_link_init());

    // Initialize WiFi AP
    esp_netif_t *wifi_netif;
    wifi_init_softap(&wifi_netif);

    // Bridge Ethernet and WiFi AP together, with one DHCP server on the bridge
    esp_netif_t *br_netif = NULL;
    setup_bridge(eth_netif, wifi_netif, common_mac, &br_netif);

    // Hooks the eth/wifi bridge ports for per-client traffic accounting.
    // Must run before esp_eth_start()/esp_wifi_start() below - see
    // client_track.h for why the ordering matters.
    ESP_ERROR_CHECK(client_track_init(eth_netif, wifi_netif, br_netif, common_mac));

    // System monitor (CPU load) for the web UI - no ordering dependency,
    // just needs the scheduler running.
    ESP_ERROR_CHECK(sys_monitor_init());

    // Admin credential RAM cache - must be up before web_server_start()/
    // serial_console_init() below, since both can call auth_cfg_load/save.
    ESP_ERROR_CHECK(auth_cfg_init());

    // Since MAC forwarding is done in the lwIP bridge, the Ethernet MAC needs
    // to pass through frames not addressed to it.
    bool promiscuous = true;
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_PROMISCUOUS, &promiscuous));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Leave the witness set for the next boot to find. Not ESP_ERROR_CHECK'd for
    // the same reason the reset history is not: a PHY that will not hold the
    // pattern costs a diagnostic, not the bridge.
    rail_witness_arm(eth_handle);

    // On-air transmit accounting. Unlike the port hooks above this one has to
    // come *after* esp_wifi_start(), which is why it is a separate call.
    ESP_ERROR_CHECK(client_track_wifi_txdone_init());

    // Start web server
    web_server_start();

    // Interactive command console over the same UART used for log output -
    // needs WiFi/NVS/client tracking already up since its commands depend
    // on them.
    ESP_ERROR_CHECK(serial_console_init());

    // Mark this boot as good now that every startup-critical subsystem above
    // has come up without hitting an ESP_ERROR_CHECK abort. Cancels the
    // bootloader's pending-verify rollback state so a good OTA image isn't
    // auto-reverted. On a non-OTA boot (fresh serial flash, blank otadata)
    // this legitimately returns an error, which is expected, not a fault.
    esp_err_t ota_mark_ret = esp_ota_mark_app_valid_cancel_rollback();
    if (ota_mark_ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA rollback: app marked valid, pending-verify cleared");
    } else {
        ESP_LOGD(TAG, "OTA rollback: mark-valid not applicable here (%s)",
                 esp_err_to_name(ota_mark_ret));
    }

    // Startup got all the way through - mark-valid above is the last thing in it
    // that can go wrong. Costs no flash, just a word of RTC memory, unlike the
    // uptime checkpoint the reset history also keeps. What it buys the next boot:
    // the difference between a device that crashed after running for ninety
    // seconds and one that never finished starting.
    reset_log_note_ready();

    ESP_LOGI(TAG, "\n====================================");
    ESP_LOGI(TAG, "Bridge Ready!");
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "Web Interface: http://192.168.5.1");
    ESP_LOGI(TAG, "Network: 192.168.5.0/24");
    ESP_LOGI(TAG, "Gateway: 192.168.5.1");
    // The requested range is the effective one now that dhcp_server.c serves
    // it - there is no validation step in between that can quietly substitute
    // a different one, as there was when ESP-IDF's server owned the pool.
    ESP_LOGI(TAG, "DHCP Pool (WiFi):     %s - %s", WIFI_DHCP_START, WIFI_DHCP_END);
    ESP_LOGI(TAG, "DHCP Pool (Ethernet): %s - %s", ETH_DHCP_START, ETH_DHCP_END);
    ESP_LOGI(TAG, "  %d reservation(s) restored from flash",
             dhcp_server_get_restored_count());
    ESP_LOGI(TAG, "====================================\n");
}