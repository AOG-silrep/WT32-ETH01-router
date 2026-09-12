#include "status_led.h"
#include "eth_link.h"
#include "wan.h"
#include "client_track.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include <stdbool.h>

// Pin the LED is wired to: pin -> resistor -> LED -> GND, so driving it high
// lights it. GPIO19 was the first choice but is the ESP32's fixed-function
// RMII TXD0 line on this board (net IO19_RMII_EMAC_TXD0 in
// docs/WT32_ETH01_V2.schematic.pdf) - it's wired straight to the LAN8720 PHY,
// not reassignable the way MDC/MDIO are, and toggling it as a GPIO would
// corrupt outgoing Ethernet frames. GPIO14 is broken out on the module
// header (EX_IO14) and otherwise unused.
#define STATUS_LED_GPIO GPIO_NUM_14

#define HEALTHY_HALF_PERIOD_US (500 * 1000)  // 1 Hz
#define FAULT_HALF_PERIOD_US   (100 * 1000)  // 5 Hz

// Set only from status_led_tick() (the sys_monitor 1Hz loop), read only from
// the blink timer callback (the esp_timer task). A plain bool is a single
// aligned byte, so this can't tear; nothing here is on the packet forwarding
// path the way client_track's counters are, so there's no throughput reason
// to avoid a lock either - there's just nothing a lock would protect.
static volatile bool s_fault;
static esp_timer_handle_t s_blink_timer;
static bool s_led_on;

static void blink_timer_cb(void *arg)
{
    s_led_on = !s_led_on;
    gpio_set_level(STATUS_LED_GPIO, s_led_on ? 1 : 0);
    esp_timer_start_once(s_blink_timer, s_fault ? FAULT_HALF_PERIOD_US : HEALTHY_HALF_PERIOD_US);
}

esp_err_t status_led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(STATUS_LED_GPIO, 0);
    s_led_on = false;

    const esp_timer_create_args_t timer_args = {
        .callback = &blink_timer_cb,
        .name = "status_led",
    };
    err = esp_timer_create(&timer_args, &s_blink_timer);
    if (err != ESP_OK) {
        return err;
    }
    return esp_timer_start_once(s_blink_timer, HEALTHY_HALF_PERIOD_US);
}

void status_led_tick(void)
{
    eth_link_status_t eth;
    eth_link_get_status(&eth);
    bool eth_fault = !eth.up || eth.speed_mbit < 100 || !eth.full_duplex;

    wan_status_t wan;
    wan_get_status(&wan);
    bool wan_fault = wan.state != WAN_STATE_DISABLED && wan.state != WAN_STATE_UP;

    bool ip_conflict = client_track_get_armed_conflicts() > 0;

    s_fault = eth_fault || wan_fault || ip_conflict;
}
