#pragma once

#include "esp_http_server.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the HTTP server on the bridge's IP (192.168.5.1), serving the
// config/status page at "/" and the JSON API under "/api/*". Must be
// called after esp_wifi_start()/esp_eth_start().
httpd_handle_t web_server_start(void);

#ifdef __cplusplus
}
#endif
