# wt32-bridge

An ESP-IDF firmware project that turns a [WT32-ETH01](https://en.wireless-tag.com/product-item-2.html) (ESP32 + LAN8720 Ethernet PHY) module into an Ethernet↔WiFi bridge for [AgOpenGPS](https://github.com/AgOpenGPS-Official/AgOpenGPS). It bridges the wired Ethernet interface and a WiFi access point at the network layer, and serves a small web UI for configuration and live monitoring.

## Features

- Ethernet (LAN8720 PHY) ↔ WiFi AP network bridge (`esp_netif_br_glue`)
- Web UI (`main/webpage/index.html`) for WiFi configuration and live status
- Connected-client tracking and per-device traffic graphing
- System monitor (heap, uptime, etc.) surfaced on the web UI, refreshed every second
- Configurable WiFi channel (1, 6, or 11)

## Building and flashing

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) (v5.3+).

```sh
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor
```

## Source layout

- `main/main.c` — startup, Ethernet/WiFi bridge setup
- `main/wifi_cfg.c` / `.h` — WiFi AP configuration (SSID, channel, credentials)
- `main/web_server.c` / `.h` — HTTP server backing the web UI
- `main/client_track.c` / `.h` — connected-client and traffic tracking
- `main/sys_monitor.c` / `.h` — system stats (heap, uptime) for the web UI
- `main/webpage/index.html` — the web UI itself
