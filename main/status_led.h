#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// A single GPIO LED that reports the bridge's health at a glance: slow blink
// while healthy, fast blink while something tracked elsewhere in the
// firmware is actually wrong. It adds no diagnostics of its own - it only
// reads state that eth_link.c, wan.c and client_track.c already maintain.

// Configures the LED pin as an output (off) and starts the blink timer.
// Call once, any time after the GPIO driver is available - it does not read
// any of the fault sources itself, so it has no ordering dependency on them.
esp_err_t status_led_init(void);

// Re-evaluates the fault sources and updates the blink pattern accordingly.
// Meant to be called once a second from sys_monitor.c's borrowed 1 Hz loop,
// the same way that loop already hosts reset_log_checkpoint_tick(),
// rail_witness_tick() and clock_time_tick() - see the note there for why.
void status_led_tick(void);

#ifdef __cplusplus
}
#endif
