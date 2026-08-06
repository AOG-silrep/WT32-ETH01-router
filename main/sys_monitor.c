#include <string.h>
#include <stdbool.h>
#include "sys_monitor.h"
#include "reset_log.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "sys_monitor";

#define TICK_PERIOD_MS 1000
#define MAX_TASKS      40

static SemaphoreHandle_t s_mutex;
static uint8_t s_cpu_pct[SYS_MONITOR_NUM_CORES];
static uint32_t s_prev_idle_rt[SYS_MONITOR_NUM_CORES];
static uint32_t s_prev_total_rt;
static bool s_have_prev;

// esp_timer_get_time() is driven by a hardware timer independent of the CPU
// clock, so timing a cycle-count delta against it yields the CPU's actual
// running frequency rather than just its configured one.
static uint32_t s_prev_cycles;
static int64_t s_prev_time_us;
static uint32_t s_cpu_freq_mhz;

// Static, not stack-local: MAX_TASKS TaskStatus_t entries (~2KB) would eat
// too much of sys_monitor_task's 4096-byte stack. Only ever touched from
// that one task, so no locking needed for this buffer itself.
static TaskStatus_t s_tasks[MAX_TASKS];

static void sys_monitor_tick(void)
{
    // Snapshot the cycle counter (this core, runs at the actual CPU clock)
    // against esp_timer_get_time() (a separate hardware timer, unaffected by
    // CPU frequency) so their ratio over this tick gives the measured MHz.
    int64_t now_us = esp_timer_get_time();
    uint32_t cycles = esp_cpu_get_cycle_count();

    uint32_t total_rt = 0;
    UBaseType_t n = uxTaskGetSystemState(s_tasks, MAX_TASKS, &total_rt);
    if (n == 0) {
        // Array too small for the current task count, or otherwise failed -
        // uxTaskGetSystemState() didn't fill anything in. Skip this tick
        // rather than compute deltas against garbage/zero state.
        return;
    }

    uint32_t idle_rt[SYS_MONITOR_NUM_CORES] = {0};
    for (UBaseType_t i = 0; i < n; i++) {
        const char *name = s_tasks[i].pcTaskName;
        if (strncmp(name, "IDLE", 4) == 0 &&
            name[4] >= '0' && name[4] < '0' + SYS_MONITOR_NUM_CORES && name[5] == '\0') {
            idle_rt[name[4] - '0'] = s_tasks[i].ulRunTimeCounter;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_have_prev && total_rt != s_prev_total_rt) {
        uint32_t total_delta = total_rt - s_prev_total_rt;
        for (int c = 0; c < SYS_MONITOR_NUM_CORES; c++) {
            uint32_t idle_delta = idle_rt[c] - s_prev_idle_rt[c];
            s_cpu_pct[c] = idle_delta >= total_delta ? 0 : (uint8_t)(100 - (idle_delta * 100 / total_delta));
        }
    }
    if (s_have_prev) {
        int64_t elapsed_us = now_us - s_prev_time_us;
        if (elapsed_us > 0) {
            // Both counters are 32/64-bit unsigned deltas over a ~1s window,
            // far short of the cycle counter's ~17s (240MHz) wraparound.
            uint32_t cycle_delta = cycles - s_prev_cycles;
            s_cpu_freq_mhz = (uint32_t)((uint64_t)cycle_delta / (uint64_t)elapsed_us);
        }
    }
    memcpy(s_prev_idle_rt, idle_rt, sizeof(idle_rt));
    s_prev_total_rt = total_rt;
    s_prev_cycles = cycles;
    s_prev_time_us = now_us;
    s_have_prev = true;
    xSemaphoreGive(s_mutex);
}

static void sys_monitor_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TICK_PERIOD_MS));
        sys_monitor_tick();

        // Borrowing this loop rather than giving reset_log a task of its own: it
        // needs a once-a-second call from somewhere that is allowed to block on a
        // flash write, and this is already that, at a priority that yields to the
        // forwarding path. reset_log's own 1 Hz timer cannot host it - the write
        // would stall the timer task that samples the uptime being written. The
        // cost lands here, on the few ticks per boot that hit a scheduled
        // checkpoint, as ~20ms of extra elapsed time - folded into the
        // FOLLOWING tick's CPU-load window, since the sample above is taken
        // before the write rather than after.
        reset_log_checkpoint_tick();
    }
}

esp_err_t sys_monitor_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(sys_monitor_task, "sys_monitor", 4096,
                                             NULL, tskIDLE_PRIORITY + 1, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sys_monitor task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void sys_monitor_get_cpu_load(uint8_t out_pct[SYS_MONITOR_NUM_CORES])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out_pct, s_cpu_pct, sizeof(s_cpu_pct));
    xSemaphoreGive(s_mutex);
}

uint32_t sys_monitor_get_cpu_freq_mhz(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t mhz = s_cpu_freq_mhz;
    xSemaphoreGive(s_mutex);
    return mhz;
}
