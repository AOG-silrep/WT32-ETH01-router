#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_eth_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

// Whether the 3.3V rail survived the reset that started this boot.
//
// The problem this exists for: on the ESP32 a reset driven at the EN pin - a
// serial adapter's DTR line, a reset button - is indistinguishable from a power
// cycle. EN low powers down the whole RTC domain, so the RTC reports
// RESET_REASON_CHIP_POWER_ON for both and ESP-IDF maps that to
// ESP_RST_POWERON. ESP_RST_EXT is not reachable on this port at all. RTC memory
// goes with it, which is why a DTR reset also loses its intent, its
// reached-ready bit and its exact uptime. No amount of reading chip registers
// separates the two cases, and the reset history has to call both "power-on or
// power loss" - the same bucket it asks the reader to diagnose as a supply
// fault when it repeats.
//
// What does separate them is off-chip: on an EN reset the 3.3V rail never
// drops. The LAN8720 hangs off that rail and main.c configures the PHY driver
// with reset_gpio_num = -1, so the firmware never hardware-resets it. Its
// interrupt-mask register (0x1E) is declared by the LAN87xx driver and never
// read or written by it, which makes it a scratch register nothing else
// competes for. Arm it with a pattern, and whether that pattern is still there
// on the next boot is a witness to what the rail did while the CPU was in
// reset.
//
// The limit, which every surface has to respect: the witness proves the rail
// stayed above the PHY's own retention threshold, not that a human pressed
// anything. A sag deep enough to reset the ESP32 (POR around 2.4V) but not deep
// enough to wipe the LAN8720 reads as HELD. So HELD is evidence for an EN reset
// and never proof of one, and nothing built on this may label a record a
// developer reset.
typedef enum {
    RAIL_WITNESS_UNKNOWN = 0,  // no usable answer - MDIO did not respond, or
                               // the previous boot never armed the witness
    RAIL_WITNESS_HELD,         // the pattern survived: 3.3V never dropped
    RAIL_WITNESS_DROPPED,      // the PHY came up cleared: this was a power event
} rail_witness_t;

// Pins the witness talks to. Taken as configuration rather than compiled in so
// main.c stays the one place this board's Ethernet pinout is written down.
typedef struct {
    int     mdc_gpio;
    int     mdio_gpio;
    int     power_gpio;  // PHY power/clock enable, or -1 if the PHY needs none
    uint8_t phy_addr;
} rail_witness_config_t;

// Reads the witness left by the previous boot. Call exactly once, and call it
// EARLY - before esp_eth_driver_install(), whose PHY init soft-resets the chip
// over BMCR and clears 0x1E along with everything else.
//
// Drives power_gpio high and polls for the PHY identifier before reading, so a
// PHY that is only now powering up gets the time it needs; a rail that held
// answers in a few milliseconds because the PHY was never off. If the
// identifier never reads back, MDIO is not working and the answer is UNKNOWN -
// never DROPPED. That distinction is the whole discipline here: a failed read
// and a real power cycle both produce zeroes, and reporting one as the other
// would turn "no data" into a claim.
//
// Bit-bangs MDIO itself rather than going through the driver, because by the
// time a driver exists it has already destroyed what this reads. Allocates
// nothing, bounds every loop, and cannot fail in a way that stops the boot -
// it runs ahead of the reset history's own record being written to flash.
rail_witness_t rail_witness_probe(const rail_witness_config_t *cfg);

// Arms the witness for the NEXT boot. Call once after esp_eth_start(), since
// the driver's PHY reset during install clears the register.
//
// Verifies the pattern by reading it back, and falls back to a second candidate
// if the first will not stick, so which bits of 0x1E this particular PHY holds
// is settled on the device rather than assumed from a datasheet. Returns
// ESP_ERR_NOT_SUPPORTED if neither candidate holds, which means the witness is
// unusable on this board and every later probe will read UNKNOWN.
esp_err_t rail_witness_arm(esp_eth_handle_t eth_handle);

// Rewrites the pattern if enough time has passed. Call about once a second from
// an ordinary task; it is a no-op on most calls.
//
// Arming once at boot would be enough if nothing ever reset the PHY again, and
// today nothing does. This costs one MDIO write every thirty seconds - tens of
// microseconds, serialised by the driver's own lock - and buys immunity from
// any future path that re-inits the PHY, where the failure would otherwise be
// silent and would show up as a power cycle that never happened.
void rail_witness_tick(void);

#ifdef __cplusplus
}
#endif
