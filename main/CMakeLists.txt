idf_component_register(SRCS "serial_console.c" "web_server.c" "client_track.c" "dhcp_server.c" "sys_monitor.c" "eth_link.c" "reset_log.c" "rail_witness.c" "wifi_cfg.c" "auth_cfg.c" "log_buf.c" "syslog.c" "syslog_cfg.c" "wan.c" "wan_cfg.c" "clock_cfg.c" "clock_time.c" "main.c"
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "webpage/index.html" "webpage/admin.html" "webpage/logs.html"
                                   "webpage/leases.html" "webpage/resets.html"
                                   "webpage/wan.html" "webpage/lan.html"
                                   "webpage/ports.html"
                    # EMBED_FILES, not EMBED_TXTFILES: the latter appends a NUL
                    # so the HTML above can be sent as a C string, and a NUL
                    # glued to the end of a PNG is a corrupt PNG. The length is
                    # therefore end - start here, with no -1.
                    EMBED_FILES "webpage/favicon.png"
                    REQUIRES esp_http_server nvs_flash esp_wifi esp_netif esp_eth lwip
                             esp_driver_gpio esp_timer app_update console)
