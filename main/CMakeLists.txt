idf_component_register(
    SRCS
        "app_main.c"
        "uart_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer
)
