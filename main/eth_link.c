#include "eth_link.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "eth_link";

// Guarded state. Written from the default event loop task, read from the
// HTTP server and console tasks, so it takes a mutex - the same shape as
// sys_monitor.c. Deliberately not the bare-volatile counter style used in
// client_track.c: that one buys its way out of locking because it sits on
// the packet hot path, which link events emphatically do not.
static SemaphoreHandle_t s_mutex;
static bool s_up;
static uint32_t s_speed_mbit;
static bool s_full_duplex;
static bool s_autoneg;
static uint32_t s_flaps;
static int64_t s_change_us;

static void on_eth_connected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)data;

    // The PHY publishes speed and duplex to the driver *before* it posts the
    // link-up event (esp_eth_phy_802_3.c drives ETH_STATE_SPEED and
    // ETH_STATE_DUPLEX ahead of ETH_STATE_LINK), so by the time we get here
    // these read back the values just negotiated. Both getters are plain
    // reads of the driver struct - no MDIO transaction, nothing that would
    // be rude to do from the event loop.
    eth_speed_t speed = ETH_SPEED_10M;
    eth_duplex_t duplex = ETH_DUPLEX_HALF;
    bool autoneg = false;
    esp_eth_ioctl(eth_handle, ETH_CMD_G_SPEED, &speed);
    esp_eth_ioctl(eth_handle, ETH_CMD_G_DUPLEX_MODE, &duplex);
    esp_eth_ioctl(eth_handle, ETH_CMD_G_AUTONEGO, &autoneg);

    uint32_t speed_mbit = (speed == ETH_SPEED_100M) ? 100 : 10;
    bool full_duplex = (duplex == ETH_DUPLEX_FULL);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_up = true;
    s_speed_mbit = speed_mbit;
    s_full_duplex = full_duplex;
    s_autoneg = autoneg;
    s_change_us = esp_timer_get_time();
    xSemaphoreGive(s_mutex);

    // Anything short of 100 Mbit full duplex still passes traffic, so it will
    // never surface as an error - it just makes the bridge slow. Say so at
    // warning level, since by the time someone is looking, the symptom they
    // have is "the bridge is slow" and this is the answer.
    if (speed_mbit == 100 && full_duplex) {
        ESP_LOGI(TAG, "eth link up: 100 Mbit full duplex");
    } else {
        ESP_LOGW(TAG, "eth link up degraded: %u Mbit %s duplex - expected 100 Mbit full, check the cable",
                 (unsigned)speed_mbit, full_duplex ? "full" : "half");
    }
}

static void on_eth_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_up = false;
    // Cleared rather than kept, because the driver's idea of speed/duplex
    // while down is its power-on default of 10M/half - which is exactly the
    // fault this module exists to spot, and would be reported as if real.
    s_speed_mbit = 0;
    s_full_duplex = false;
    // Counted here and only here: one flap is one loss of link, so counting
    // the up edge as well would double every unplug.
    s_flaps++;
    s_change_us = esp_timer_get_time();
    xSemaphoreGive(s_mutex);

    ESP_LOGW(TAG, "eth link down");
}

esp_err_t eth_link_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // Seeded now so the "last change" age counts from boot rather than from
    // the epoch on a device that never sees a cable.
    s_change_us = esp_timer_get_time();

    // No ordering dependency on the bridge glue, unlike the ETH_EVENT
    // registration in client_track_init() - this module only reads driver
    // state and never touches netif->input.
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_CONNECTED,
                                                &on_eth_connected, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED,
                                                &on_eth_disconnected, NULL));
    return ESP_OK;
}

void eth_link_get_status(eth_link_status_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    out->up = s_up;
    out->speed_mbit = s_speed_mbit;
    out->full_duplex = s_full_duplex;
    out->autoneg = s_autoneg;
    out->flaps = s_flaps;
    out->since_change_s = (uint32_t)((esp_timer_get_time() - s_change_us) / 1000000);
    xSemaphoreGive(s_mutex);
}
