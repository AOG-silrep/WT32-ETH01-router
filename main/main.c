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
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/ip_addr.h"
#include "dhcpserver/dhcpserver.h"
#include "netif/ethernet.h"
#include <sys/socket.h>

static const char *TAG = "AOG-BRIDGE";

// WiFi AP Configuration
#define WIFI_SSID      "AOG hub"
#define WIFI_PASS      "password"
#define WIFI_CHANNEL   1
#define MAX_STA_CONN   6

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

// Global handles
esp_netif_ip_info_t ip;
static esp_netif_t *ap_netif = NULL;

// NVS Keys
#define NVS_NAMESPACE "wifi_config"
#define NVS_SSID_KEY  "ssid"
#define NVS_PASS_KEY  "password"

// Load WiFi settings from NVS
static void load_wifi_config(char *ssid, char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    
    if(err == ESP_OK){
        size_t ssid_len = 32;
        size_t pass_len = 64;
        
        err = nvs_get_str(nvs_handle, NVS_SSID_KEY, ssid, &ssid_len);
        if(err != ESP_OK){
            strcpy(ssid, WIFI_SSID);
        }
        
        err = nvs_get_str(nvs_handle, NVS_PASS_KEY, password, &pass_len);
        if(err != ESP_OK){
            strcpy(password, WIFI_PASS);
        }
        
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Loaded WiFi config - SSID: %s", ssid);
    }
    else{
        strcpy(ssid, WIFI_SSID);
        strcpy(password, WIFI_PASS);
        ESP_LOGI(TAG, "Using default WiFi config");
    }
}

static void setup_dhcp_server(void)
{
    ESP_LOGI(TAG, "Configuring DHCP Server...");

    // Create default AP netif configuration
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_AP();
    
    // Create the netif instance
    esp_netif_t *br_netif = esp_netif_new(&cfg);
    if (br_netif == NULL) {
        ESP_LOGE(TAG, "Failed to get bridge netif!");
        return;
    }
    
    esp_netif_dhcps_stop(br_netif);

    // Set static IP for the bridge interface
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(esp_netif_ip_info_t));
    inet_pton(AF_INET, BRIDGE_IP, &ip_info.ip);
    inet_pton(AF_INET, BRIDGE_NETMASK, &ip_info.netmask);
    inet_pton(AF_INET, BRIDGE_GW, &ip_info.gw);
    esp_netif_set_ip_info(br_netif, &ip_info);

    // Configure DHCP server
    dhcps_lease_t lease;
    lease.enable = true;
    inet_pton(AF_INET, DHCP_START, &lease.start_ip);
    inet_pton(AF_INET, DHCP_END, &lease.end_ip);

    esp_netif_dhcps_option(br_netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease));
    esp_err_t err = esp_netif_dhcps_start(br_netif);
    
    if(err == ESP_OK){
        ESP_LOGI(TAG, "DHCP Server Started");
        ESP_LOGI(TAG, "  IP Pool: %s - %s", DHCP_START, DHCP_END);
    }
    else{
        ESP_LOGE(TAG, "Failed to start DHCP Server: %d", err);
    }
}

static void wifi_init_softap(void)
{
    char ssid[32];
    char password[64];
    
    load_wifi_config(ssid, password);

    // Create WiFi AP netif with custom config to avoid default behavior
    esp_netif_inherent_config_t netif_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
    netif_cfg.if_desc = "wifi_ap";
    netif_cfg.route_prio = 10;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_LOGI(TAG, "Initializing WiFi Access Point...");
    esp_netif_config_t cfg_ap = {
        .base = &netif_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_AP,
    };
    ap_netif = esp_netif_new(&cfg_ap);
    assert(ap_netif);

    // Configure static IP for AP
    esp_netif_dhcps_stop(ap_netif);
    
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(esp_netif_ip_info_t));
    ip_info.ip.addr = esp_ip4addr_aton(BRIDGE_IP);
    ip_info.gw.addr = esp_ip4addr_aton(BRIDGE_IP);
    ip_info.netmask.addr = esp_ip4addr_aton(BRIDGE_NETMASK);
    
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);
    
    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(ssid),
            .channel = WIFI_CHANNEL,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    strcpy((char*)wifi_config.ap.ssid, ssid);
    strcpy((char*)wifi_config.ap.password, password);
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi AP Started");
    ESP_LOGI(TAG, "  SSID: %s", ssid);
    ESP_LOGI(TAG, "  Password: %s", password);
    ESP_LOGI(TAG, "  IP: %s", BRIDGE_IP);
    
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void eth_init(void) {
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

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    
    ESP_LOGI(TAG, "Ethernet initialization complete");
    ESP_LOGI(TAG, "  Configured IP: %s (bridged with WiFi AP)", BRIDGE_IP);
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

    // Initialize WiFi AP first
    wifi_init_softap();
    
    // Initialize Ethernet
    eth_init();

    setup_dhcp_server();
    
    // Start web server
    //server = start_webserver();
    
    ESP_LOGI(TAG, "\n====================================");
    ESP_LOGI(TAG, "Bridge Ready!");
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "Web Interface: http://192.168.5.1");
    ESP_LOGI(TAG, "Network: 192.168.5.0/24");
    ESP_LOGI(TAG, "Gateway: 192.168.5.1");
    ESP_LOGI(TAG, "DHCP Pool: 192.168.5.2-254");
    ESP_LOGI(TAG, "====================================\n");
}