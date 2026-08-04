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
#include "dhcpserver/dhcpserver.h"
#include "netif/ethernet.h"
#include <sys/socket.h>
#include "wifi_cfg.h"
#include "client_track.h"
#include "sys_monitor.h"
#include "auth_cfg.h"
#include "web_server.h"
#include "serial_console.h"
#include "log_buf.h"

static const char *TAG = "AOG-BRIDGE";

// Network Configuration
#define BRIDGE_IP      "192.168.5.1"
#define BRIDGE_NETMASK "255.255.255.0"
#define BRIDGE_GW      "192.168.5.1"
#define DHCP_START     "192.168.5.2"
// Not .254. A range wider than DHCPS_MAX_LEASE (100, fixed in ESP-IDF's
// dhcpserver.h) is rejected outright by esp_netif_dhcps_option(), which then
// leaves the server on its own default range - so asking for .2-.254 got a
// narrower pool than asking for .2-.101 does, and said nothing about it.
#define DHCP_END       "192.168.5.101"

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
    ESP_LOGI(TAG, "Configuring bridge + DHCP Server...");

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

    // Configure the DHCP lease range before the bridge attach below starts
    // the DHCP server automatically.
    //
    // The result is checked because this call has a silent failure mode that
    // this code sat in for some time: esp_netif_dhcps_option() validates the
    // range and returns ESP_ERR_ESP_NETIF_INVALID_PARAMS without storing
    // anything if it is wider than DHCPS_MAX_LEASE, out of subnet, or covers
    // the bridge's own address. The DHCP server then quietly runs on a default
    // range of its own choosing, which is not the one logged below.
    dhcps_lease_t lease;
    lease.enable = true;
    inet_pton(AF_INET, DHCP_START, &lease.start_ip);
    inet_pton(AF_INET, DHCP_END, &lease.end_ip);
    esp_err_t lease_err = esp_netif_dhcps_option(br_netif, ESP_NETIF_OP_SET,
                                                  ESP_NETIF_REQUESTED_IP_ADDRESS,
                                                  &lease, sizeof(lease));
    if (lease_err != ESP_OK) {
        ESP_LOGE(TAG, "DHCP lease range %s-%s refused (%s) - the server will use "
                      "a default range instead", DHCP_START, DHCP_END,
                 esp_err_to_name(lease_err));
    }

    esp_netif_br_glue_handle_t br_glue = esp_netif_br_glue_new();
    ESP_ERROR_CHECK(esp_netif_br_glue_add_port(br_glue, eth_netif));
    ESP_ERROR_CHECK(esp_netif_br_glue_add_wifi_port(br_glue, wifi_netif));
    // Attaching starts the bridge (and, with it, the DHCP server) immediately.
    ESP_ERROR_CHECK(esp_netif_attach(br_netif, br_glue));

    ESP_LOGI(TAG, "Bridge + DHCP Server started");

    // Read the range back rather than printing DHCP_START/DHCP_END. What was
    // asked for and what the server ended up with are not the same thing when
    // the request is refused, and the whole point of this line is to say which
    // addresses clients will actually be given.
    dhcps_lease_t in_use;
    if (esp_netif_dhcps_option(br_netif, ESP_NETIF_OP_GET, ESP_NETIF_REQUESTED_IP_ADDRESS,
                               &in_use, sizeof(in_use)) == ESP_OK) {
        // dhcps_lease_t carries lwIP's ip4_addr_t, while esp_ip4addr_ntoa()
        // takes esp_netif's esp_ip4_addr_t. Same 32 bits, different declared
        // type, so copy the address across rather than casting the pointer.
        esp_ip4_addr_t start_ip = { .addr = in_use.start_ip.addr };
        esp_ip4_addr_t end_ip = { .addr = in_use.end_ip.addr };
        char start_str[16], end_str[16];
        esp_ip4addr_ntoa(&start_ip, start_str, sizeof(start_str));
        esp_ip4addr_ntoa(&end_ip, end_str, sizeof(end_str));
        ESP_LOGI(TAG, "  IP Pool: %s - %s", start_str, end_str);
    } else {
        ESP_LOGW(TAG, "  IP Pool: could not be read back");
    }

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

    ESP_LOGI(TAG, "\n====================================");
    ESP_LOGI(TAG, "Bridge Ready!");
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "Web Interface: http://192.168.5.1");
    ESP_LOGI(TAG, "Network: 192.168.5.0/24");
    ESP_LOGI(TAG, "Gateway: 192.168.5.1");
    // Hardcoded literals here would be a second place to get this wrong; the
    // range setup_bridge() read back and logged is the authoritative one.
    ESP_LOGI(TAG, "DHCP Pool: %s - %s (see \"IP Pool\" above for what took effect)",
             DHCP_START, DHCP_END);
    ESP_LOGI(TAG, "====================================\n");
}