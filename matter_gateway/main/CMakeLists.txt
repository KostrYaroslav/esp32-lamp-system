idf_component_register(
    SRCS 
        "main.c"
        "uart_handler.c"
    INCLUDE_DIRS "."
    REQUIRES 
        esp_driver_uart
        nvs_flash
        esp_netif
        esp_event
)