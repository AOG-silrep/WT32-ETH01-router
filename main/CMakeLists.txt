idf_component_register(SRCS "web_server.c" "client_track.c" "sys_monitor.c" "wifi_cfg.c" "auth_cfg.c" "main.c"
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "webpage/index.html" "webpage/admin.html"
                    REQUIRES esp_http_server nvs_flash esp_wifi esp_netif esp_eth lwip
                             esp_driver_gpio esp_timer app_update)
