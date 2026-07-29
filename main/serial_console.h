#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts an interactive command console (REPL) on the same UART used by
// esp_log output (the only UART exposed on the WT32-ETH01 board). Lowers
// the default log level so routine INFO logs don't clutter the prompt; use
// the "loglevel" console command to raise it back. Must be called after
// WiFi/NVS/client tracking are up, since several commands depend on them.
esp_err_t serial_console_init(void);

#ifdef __cplusplus
}
#endif
