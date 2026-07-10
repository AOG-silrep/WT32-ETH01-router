#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
//#include "esp_bridge.h"
//#include "esp_bridge_internal.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_eth_phy_lan87xx.h"
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
#include "web_server.h"

static const char *TAG = "AOG-BRIDGE";

// Network Configuration
#define BRIDGE_IP      "192.168.5.1"
#define BRIDGE_NETMASK "255.255.255.0"
#define BRIDGE_GW      "192.168.5.1"
#define DHCP_START     "192.168.5.2"
#define DHCP_END       "192.168.5.254"

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

    bridgeif_config_t bridge_config = {
        .max_fdb_dyn_entries = 10,
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
    dhcps_lease_t lease;
    lease.enable = true;
    inet_pton(AF_INET, DHCP_START, &lease.start_ip);
    inet_pton(AF_INET, DHCP_END, &lease.end_ip);
    esp_netif_dhcps_option(br_netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease));

    esp_netif_br_glue_handle_t br_glue = esp_netif_br_glue_new();
    ESP_ERROR_CHECK(esp_netif_br_glue_add_port(br_glue, eth_netif));
    ESP_ERROR_CHECK(esp_netif_br_glue_add_wifi_port(br_glue, wifi_netif));
    // Attaching starts the bridge (and, with it, the DHCP server) immediately.
    ESP_ERROR_CHECK(esp_netif_attach(br_netif, br_glue));

    ESP_LOGI(TAG, "Bridge + DHCP Server started");
    ESP_LOGI(TAG, "  IP Pool: %s - %s", DHCP_START, DHCP_END);

    *out_br_netif = br_netif;
}

void app_main(void)
{
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

    // Since MAC forwarding is done in the lwIP bridge, the Ethernet MAC needs
    // to pass through frames not addressed to it.
    bool promiscuous = true;
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_PROMISCUOUS, &promiscuous));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Start web server
    web_server_start();

    ESP_LOGI(TAG, "\n====================================");
    ESP_LOGI(TAG, "Bridge Ready!");
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "Web Interface: http://192.168.5.1");
    ESP_LOGI(TAG, "Network: 192.168.5.0/24");
    ESP_LOGI(TAG, "Gateway: 192.168.5.1");
    ESP_LOGI(TAG, "DHCP Pool: 192.168.5.2-254");
    ESP_LOGI(TAG, "====================================\n");
}