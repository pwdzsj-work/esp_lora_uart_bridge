idf_component_register(
    SRCS
        "app_main.c"
        "maintenance_portal.c"
        "status_led.c"
        "uart_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        driver
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        nvs_flash
)
