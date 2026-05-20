#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "UART_HANDLER";

#define UART_MAIN_PORT      UART_NUM_1
#define UART_MAIN_TXD       4
#define UART_MAIN_RXD       5
#define BUF_SIZE            256

static QueueHandle_t tx_queue = NULL;

void uart_init(void) {
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    uart_param_config(UART_MAIN_PORT, &uart_config);
    uart_set_pin(UART_MAIN_PORT, UART_MAIN_TXD, UART_MAIN_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_MAIN_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0);
    
    tx_queue = xQueueCreate(10, sizeof(char*));
    
    ESP_LOGI(TAG, "✅ UART инициализирован (TX=%d, RX=%d)", UART_MAIN_TXD, UART_MAIN_RXD);
}

void uart_send_command(const char *cmd) {
    char *cmd_copy = strdup(cmd);
    if (cmd_copy) {
        xQueueSend(tx_queue, &cmd_copy, pdMS_TO_TICKS(100));
    }
}

static void uart_tx_task(void *pvParameters) {
    char *cmd;
    while (1) {
        if (xQueueReceive(tx_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            uart_write_bytes(UART_MAIN_PORT, cmd, strlen(cmd));
            uart_write_bytes(UART_MAIN_PORT, "\r\n", 2);
            ESP_LOGI(TAG, "📤 Отправлено Главному: %s", cmd);
            free(cmd);
        }
    }
}

static void uart_rx_task(void *pvParameters) {
    uint8_t data[BUF_SIZE];
    char line[BUF_SIZE];
    int pos = 0;

    while (1) {
        int len = uart_read_bytes(UART_MAIN_PORT, data, sizeof(data) - 1, pdMS_TO_TICKS(50));
        for (int i = 0; i < len; i++) {
            if (data[i] == '\n' || data[i] == '\r') {
                if (pos > 0) {
                    line[pos] = '\0';
                    ESP_LOGI(TAG, "📥 Получено от Главного: %s", line);
                    // Здесь будет обновление состояния Matter лампы
                    pos = 0;
                }
            } else {
                if (pos < sizeof(line) - 1) {
                    line[pos++] = data[i];
                } else {
                    pos = 0;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void uart_start_rx_task(void) {
    xTaskCreate(uart_tx_task, "uart_tx_task", 4096, NULL, 5, NULL);
    xTaskCreate(uart_rx_task, "uart_rx_task", 4096, NULL, 5, NULL);
}