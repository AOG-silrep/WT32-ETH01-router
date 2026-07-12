#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_MONITOR_NUM_CORES CONFIG_FREERTOS_NUMBER_OF_CORES

// Starts the periodic task that samples per-core CPU load from FreeRTOS
// idle-task run-time counters (requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS).
esp_err_t sys_monitor_init(void);

// Copies the most recently sampled per-core CPU load (0-100) into out_pct.
// Thread-safe; intended to be called from the HTTP server task.
void sys_monitor_get_cpu_load(uint8_t out_pct[SYS_MONITOR_NUM_CORES]);

// Returns the most recently measured actual CPU clock speed in MHz, derived
// from the CPU cycle counter timed against esp_timer_get_time(). 0 until the
// first measurement window completes. Thread-safe.
uint32_t sys_monitor_get_cpu_freq_mhz(void);

#ifdef __cplusplus
}
#endif
