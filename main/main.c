#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_bridge.h"
#include "esp_bridge_internal.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
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
static httpd_handle_t server = NULL;
static bool eth_connected = false;
static bool ap_started = false;

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

// Save WiFi settings to NVS
static void save_wifi_config(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if(err == ESP_OK){
        nvs_set_str(nvs_handle, NVS_SSID_KEY, ssid);
        nvs_set_str(nvs_handle, NVS_PASS_KEY, password);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Saved WiFi config - SSID: %s", ssid);
    }
}

// Event handler
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START){
        ESP_LOGI(TAG, "WiFi AP Started");
        ap_started = true;
    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED){
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" connected, AID: %d", MAC2STR(event->mac), event->aid);
    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED){
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" disconnected, AID: %d", MAC2STR(event->mac), event->aid);
    }
    else if(event_base == ETH_EVENT && event_id == ETHERNET_EVENT_CONNECTED){
        ESP_LOGI(TAG, "Ethernet Link Up");
        eth_connected = true;
    }
    else if(event_base == ETH_EVENT && event_id == ETHERNET_EVENT_DISCONNECTED){
        ESP_LOGI(TAG, "Ethernet Link Down");
        eth_connected = false;
    }
    else if(event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP){
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Ethernet Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// HTTP handlers
static esp_err_t root_handler(httpd_req_t *req)
{
    const char* html = "<!DOCTYPE html>"
    "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>"
    "<title>WT32-ETH01 Bridge</title><style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Arial,sans-serif;"
    "background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;padding:20px}"
    ".container{max-width:1000px;margin:0 auto}"
    ".card{background:white;border-radius:12px;padding:30px;margin-bottom:20px;"
    "box-shadow:0 10px 30px rgba(0,0,0,0.2)}"
    "h1{color:#667eea;margin-bottom:10px;font-size:28px}"
    ".subtitle{color:#666;margin-bottom:30px;font-size:14px}"
    ".status-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:15px;margin-bottom:30px}"
    ".stat-box{background:#f8f9fa;padding:20px;border-radius:8px;border-left:4px solid #667eea}"
    ".stat-label{font-size:12px;color:#666;text-transform:uppercase;margin-bottom:5px}"
    ".stat-value{font-size:24px;font-weight:bold;color:#333}"
    ".btn{background:#667eea;color:white;border:none;padding:12px 24px;border-radius:8px;"
    "cursor:pointer;font-size:14px;font-weight:600;margin-right:10px}"
    "</style></head><body><div class='container'><div class='card'>"
    "<h1>🌐 WT32-ETH01 Bridge</h1>"
    "<div class='subtitle'>Network: 192.168.5.0/24 • Gateway: 192.168.5.1</div>"
    "<div class='status-grid'>"
    "<div class='stat-box'><div class='stat-label'>Ethernet Status</div><div class='stat-value'>Connected</div></div>"
    "<div class='stat-box'><div class='stat-label'>WiFi AP Status</div><div class='stat-value'>Active</div></div>"
    "</div>"
    "<button class='btn' onclick='location.reload()'>🔄 Refresh</button>"
    "</div></div></body></html>";
    
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t root_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_handler,
    .user_ctx  = NULL
};

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting web server on port: '%d'", config.server_port);
    if(httpd_start(&server, &config) == ESP_OK){
        httpd_register_uri_handler(server, &root_uri);
        return server;
    }

    ESP_LOGE(TAG, "Error starting server!");
    return NULL;
}

static void setup_dhcp_server(void)
{
    ESP_LOGI(TAG, "Configuring DHCP Server...");

    esp_netif_t *br_netif = esp_netif_get_handle_from_ifkey("BRIDGE_DEF");
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
    
    ESP_LOGI(TAG, "Initializing WiFi Access Point...");
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
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

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch(event_id){
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "  MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
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

    // Get the bridge network interface
    //uint8_t macAddress[6];
    //esp_efuse_mac_get_default(macAddress);
    //esp_netif_t *bridge_netif = esp_bridge_create_eth_netif(&ip, &macAddress, true, false);
    //if (bridge_netif == NULL) {
        //ESP_LOGE(TAG, "Failed to get bridge netif handle!");
        //return;
    //}

    //ESP_LOGI(TAG, "Attaching Ethernet to the bridge...");
    //esp_netif_br_glue_handle_t netif_br_glue = esp_netif_br_glue_new();
    //ESP_ERROR_CHECK(esp_netif_br_glue_add_port(netif_br_glue, eth_handle));
    //ESP_ERROR_CHECK(esp_netif_attach(br_netif, esp_eth_new_netif_glue(eth_handle)));
    int err = ip_napt_enable_netif(eth_handle, 0); // Disable NAPT for ethernet interface
    if(err == 0){
        ESP_LOGE(TAG, "Failed to disable NAPT for ethernet interface: %d", err);
    }
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

    esp_bridge_create_all_netif();
    
    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    
    IP4_ADDR(&ip.ip, BRIDGE_IP[0], BRIDGE_IP[1], BRIDGE_IP[2], BRIDGE_IP[3]);
    IP4_ADDR(&ip.gw, BRIDGE_GW[0], BRIDGE_GW[1], BRIDGE_GW[2], BRIDGE_GW[3]);
    IP4_ADDR(&ip.netmask, BRIDGE_NETMASK[0], BRIDGE_NETMASK[1], BRIDGE_NETMASK[2], BRIDGE_NETMASK[3]);

    // Initialize WiFi AP first
    wifi_init_softap();
    
    // Initialize Ethernet
    eth_init();

    setup_dhcp_server();
    
    // Start web server
    server = start_webserver();
    
    ESP_LOGI(TAG, "\n====================================");
    ESP_LOGI(TAG, "Bridge Ready!");
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "Web Interface: http://192.168.5.1");
    ESP_LOGI(TAG, "Network: 192.168.5.0/24");
    ESP_LOGI(TAG, "Gateway: 192.168.5.1");
    ESP_LOGI(TAG, "DHCP Pool: 192.168.5.2-254");
    ESP_LOGI(TAG, "====================================\n");
}