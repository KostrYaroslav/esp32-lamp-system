idf_component_register(
    SRCS 
        "main.c"
        "command_parser.c"
        "state_manager.c"
        "espnow_handler.c"
        "uart_handler.c"
    INCLUDE_DIRS "."
    REQUIRES 
        esp_driver_uart
        nvs_flash
        esp_netif
        esp_event
        esp_wifi
)