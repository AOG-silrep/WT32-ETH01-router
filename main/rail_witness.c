#include "rail_witness.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rail_witness";

// Clause 22 register numbers. IMR is the one this module lives in; the two
// identifier registers are only read, to tell "the PHY says 0x1E is clear" from
// "nothing answered on the wire".
#define PHY_REG_IDR1 0x02
#define PHY_REG_IDR2 0x03
#define PHY_REG_IMR  0x1E

// OUI the LAN87xx driver itself checks for, computed the same way
// esp_eth_phy_802_3_read_oui() does. Matching its arithmetic rather than
// comparing raw register values keeps this agreeing with the driver about what
// counts as the right chip.
#define PHY_OUI_SMSC 0x1F0

// Which bits of IMR hold a value is a property of the die rather than of the
// datasheet. Measured on the LAN8720A on this board (ID2 0xC0F1) by writing
// 0xFFFF and reading back: 0x80FF. Bits 8-14 ignore writes entirely, including
// the wake-on-LAN bit the LAN87xx driver declares, which exists only on the
// LAN8740A/8742A.
//
// That leaves bit 15 and bit 0, both reserved and both writable, which is the
// pattern to prefer: it holds state without arming a single live interrupt
// mask. Bits 1-7 are real mask bits, and setting one would enable the PHY's
// nINT output for an event this firmware has no handler for - harmless on a
// board where that pin goes nowhere, but not worth relying on. They are the
// fallback, for a PHY revision where the reserved bits turn out to read back as
// zero.
//
// Either candidate works for the reader: both are non-zero, and a PHY that has
// just powered on reads 0x0000, so there is nothing ambiguous to resolve.
#define WITNESS_PATTERN_PRIMARY  0x8001
#define WITNESS_PATTERN_FALLBACK 0x00AA

// 100 kHz. The LAN8720 will take MDC at 2.5 MHz, but nothing here is in a hurry:
// one frame is 64 bits, so a read costs well under a millisecond and this runs
// a handful of times per boot.
#define MDC_HALF_PERIOD_US 5

// How long a cold PHY is given to answer before the probe gives up. The wait is
// a poll rather than a fixed delay, so the case that matters - a rail that held,
// where the PHY was never off - returns almost immediately and does not put half
// a second in front of the reset history's record being written.
#define PHY_ID_POLL_INTERVAL_MS 10
#define PHY_ID_TIMEOUT_MS       500

// Set by rail_witness_arm() once it knows which pattern this PHY holds, and read
// by the tick. Zero means the witness is not armed and the tick has nothing to
// refresh.
static uint16_t s_pattern;
static esp_eth_handle_t s_eth_handle;
static int64_t s_last_rewrite_us;

#define REWRITE_INTERVAL_US (30 * 1000 * 1000)

// ---- bit-banged MDIO ----
//
// Only used before the Ethernet driver exists. Once it does, esp_eth_ioctl()
// does this properly through the EMAC and under its lock, and this code must
// not run again - two masters on one MDIO bus is a bug, not a fallback.

static int s_mdc;
static int s_mdio;

static void mdc_pulse_out(int level)
{
    gpio_set_level(s_mdio, level);
    esp_rom_delay_us(MDC_HALF_PERIOD_US);
    gpio_set_level(s_mdc, 1);          // PHY samples MDIO on the rising edge
    esp_rom_delay_us(MDC_HALF_PERIOD_US);
    gpio_set_level(s_mdc, 0);
}

static int mdc_pulse_in(void)
{
    esp_rom_delay_us(MDC_HALF_PERIOD_US);
    gpio_set_level(s_mdc, 1);
    esp_rom_delay_us(MDC_HALF_PERIOD_US);
    int bit = gpio_get_level(s_mdio);  // stable by now; the PHY drove it on the edge
    gpio_set_level(s_mdc, 0);
    return bit;
}

static void mdio_drive(void)
{
    gpio_set_direction(s_mdio, GPIO_MODE_OUTPUT);
}

static void mdio_release(void)
{
    gpio_set_direction(s_mdio, GPIO_MODE_INPUT);
}

static void mdio_write_bits(uint32_t value, int count)
{
    for (int i = count - 1; i >= 0; i--) {
        mdc_pulse_out((value >> i) & 1);
    }
}

// Reads only. The write side of clause 22 is deliberately absent: arming happens
// through esp_eth_ioctl() once the driver owns the bus, and a second MDIO master
// bit-banging the same two pins behind the driver's back would be a bug rather
// than a fallback.
static uint16_t mdio_read_reg(uint8_t phy_addr, uint8_t reg_addr)
{
    mdio_drive();
    mdio_write_bits(0xFFFFFFFF, 32);  // preamble
    mdio_write_bits(0x1, 2);          // ST = 01
    mdio_write_bits(0x2, 2);          // OP = 10, read
    mdio_write_bits(phy_addr & 0x1F, 5);
    mdio_write_bits(reg_addr & 0x1F, 5);

    // Turnaround: one clock, not two. The PHY launches each bit on a rising edge
    // and mdc_pulse_in() samples well after that same edge, so the pulse that
    // covers the turnaround already returns the PHY's zero and the next one is
    // data bit 15. Clocking twice here reads every register shifted left by one
    // with a stray 1 in the LSB - which is exactly what this did at first, and
    // it looks like a PHY that is not there rather than like a framing bug.
    mdio_release();
    mdc_pulse_in();

    uint16_t value = 0;
    for (int i = 0; i < 16; i++) {
        value = (uint16_t)((value << 1) | mdc_pulse_in());
    }

    // One idle clock with the bus still released, so the PHY has finished
    // letting go of MDIO before the next frame drives it. Without this the
    // first read of a pair succeeds and the second returns 0xFFFF - the
    // contention does not corrupt a bit here and there, it costs the whole
    // frame, which reads as a PHY that is not present.
    mdc_pulse_in();
    return value;
}

// ---- probe ----

// Kept for the log line at the end of the probe. What separates a witness that
// is working from one that is quietly broken is whether the PHY answered at
// all, and these are the only two values that say so.
static uint16_t s_last_id1, s_last_id2;

static bool phy_responds(uint8_t phy_addr)
{
    uint16_t id1 = mdio_read_reg(phy_addr, PHY_REG_IDR1);
    uint16_t id2 = mdio_read_reg(phy_addr, PHY_REG_IDR2);
    s_last_id1 = id1;
    s_last_id2 = id2;
    uint32_t oui = ((uint32_t)id1 << 6) | (id2 >> 10);
    return oui == PHY_OUI_SMSC;
}

rail_witness_t rail_witness_probe(const rail_witness_config_t *cfg)
{
    if (cfg == NULL) {
        return RAIL_WITNESS_UNKNOWN;
    }

    // Enabling the PHY here rather than leaving it to eth_init() is not a
    // side effect to tidy up later: the PHY cannot answer MDIO with its clock
    // held off, and eth_init() runs long after the record this feeds has been
    // written. It sets the same pin high again, which costs nothing.
    if (cfg->power_gpio >= 0) {
        gpio_set_direction(cfg->power_gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(cfg->power_gpio, 1);
    }

    s_mdc = cfg->mdc_gpio;
    s_mdio = cfg->mdio_gpio;

    gpio_set_direction(s_mdc, GPIO_MODE_OUTPUT);
    gpio_set_level(s_mdc, 0);
    // Pulled up so a PHY that never answers reads as 0xFFFF rather than as
    // whatever the floating line happens to settle at. The board has its own
    // pull-up on MDIO; this only covers the case where it does not.
    gpio_set_pull_mode(s_mdio, GPIO_PULLUP_ONLY);
    mdio_release();

    rail_witness_t result = RAIL_WITNESS_UNKNOWN;
    bool answered = false;
    uint16_t imr = 0;
    int waited = 0;
    for (; waited <= PHY_ID_TIMEOUT_MS; waited += PHY_ID_POLL_INTERVAL_MS) {
        if (phy_responds(cfg->phy_addr)) {
            answered = true;
            imr = mdio_read_reg(cfg->phy_addr, PHY_REG_IMR);
            if (imr == WITNESS_PATTERN_PRIMARY || imr == WITNESS_PATTERN_FALLBACK) {
                result = RAIL_WITNESS_HELD;
            } else if (imr == 0x0000) {
                result = RAIL_WITNESS_DROPPED;
            }
            // Anything else and the PHY is talking but 0x1E holds something this
            // build did not write - firmware that never armed it, or a pattern
            // that only partly stuck. Neither is a rail reading, so the verdict
            // stays UNKNOWN and the raw value goes in the log below.
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(PHY_ID_POLL_INTERVAL_MS));
    }

    // Hand the pins back before the EMAC claims them through the GPIO matrix.
    gpio_reset_pin(s_mdc);
    gpio_reset_pin(s_mdio);
    s_mdc = -1;
    s_mdio = -1;

    // The raw register value and the wait are in the line on purpose. They are
    // what tells a witness that is working from one that is not - a PHY that
    // never answers, or one holding a pattern half of which did not stick, both
    // report UNKNOWN and would otherwise be indistinguishable in a log.
    if (!answered) {
        ESP_LOGI(TAG, "no rail reading: PHY silent after %d ms (ID1 0x%04X ID2 0x%04X)",
                 waited, s_last_id1, s_last_id2);
    } else if (result == RAIL_WITNESS_HELD) {
        ESP_LOGI(TAG, "3.3V rail held across the reset - EN pin, not a power cycle "
                      "(IMR 0x%04X after %d ms)", imr, waited);
    } else if (result == RAIL_WITNESS_DROPPED) {
        ESP_LOGI(TAG, "PHY came up cleared - the 3.3V rail dropped "
                      "(IMR 0x%04X after %d ms)", imr, waited);
    } else {
        ESP_LOGI(TAG, "no rail reading: IMR 0x%04X is not a witness (after %d ms)", imr, waited);
    }
    return result;
}

// ---- arm and refresh ----

static esp_err_t phy_reg_write(esp_eth_handle_t handle, uint16_t reg, uint32_t value)
{
    esp_eth_phy_reg_rw_data_t rw = { .reg_addr = reg, .reg_value_p = &value };
    return esp_eth_ioctl(handle, ETH_CMD_WRITE_PHY_REG, &rw);
}

static esp_err_t phy_reg_read(esp_eth_handle_t handle, uint16_t reg, uint32_t *out)
{
    esp_eth_phy_reg_rw_data_t rw = { .reg_addr = reg, .reg_value_p = out };
    return esp_eth_ioctl(handle, ETH_CMD_READ_PHY_REG, &rw);
}

static bool try_pattern(esp_eth_handle_t handle, uint16_t pattern)
{
    uint32_t readback = 0;
    if (phy_reg_write(handle, PHY_REG_IMR, pattern) != ESP_OK) {
        return false;
    }
    if (phy_reg_read(handle, PHY_REG_IMR, &readback) != ESP_OK) {
        return false;
    }
    return (uint16_t)readback == pattern;
}

esp_err_t rail_witness_arm(esp_eth_handle_t eth_handle)
{
    if (eth_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_eth_handle = eth_handle;

    if (try_pattern(eth_handle, WITNESS_PATTERN_PRIMARY)) {
        s_pattern = WITNESS_PATTERN_PRIMARY;
    } else if (try_pattern(eth_handle, WITNESS_PATTERN_FALLBACK)) {
        ESP_LOGW(TAG, "reserved IMR bits would not hold; using the wake-on-LAN bit");
        s_pattern = WITNESS_PATTERN_FALLBACK;
    } else {
        // Left unarmed rather than written blindly. A pattern that does not read
        // back is one the next boot will not find either, and a witness that
        // always reads DROPPED would report every reset as a power cycle - worse
        // than reporting none of them, because it would look like evidence.
        ESP_LOGW(TAG, "PHY register 0x1E holds nothing; rail witness unavailable");
        s_pattern = 0;
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_last_rewrite_us = esp_timer_get_time();
    ESP_LOGI(TAG, "rail witness armed with 0x%04X", s_pattern);
    return ESP_OK;
}

void rail_witness_tick(void)
{
    if (s_pattern == 0 || s_eth_handle == NULL) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (now - s_last_rewrite_us < REWRITE_INTERVAL_US) {
        return;
    }
    s_last_rewrite_us = now;
    phy_reg_write(s_eth_handle, PHY_REG_IMR, s_pattern);
}
