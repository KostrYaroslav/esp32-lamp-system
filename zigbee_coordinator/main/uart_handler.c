#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "state_manager.h"

static const char *TAG = "UART_HANDLER";

#define UART_MAIN_PORT      UART_NUM_1
#define UART_MAIN_TXD       4
#define UART_MAIN_RXD       5
#define BUF_SIZE            256
#define STATE_TIMEOUT_MS    500

static QueueHandle_t tx_queue = NULL;
static SemaphoreHandle_t state_semaphore = NULL;
static bool waiting_for_state = false;

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
    state_semaphore = xSemaphoreCreateBinary();
    
    ESP_LOGI(TAG, "✅ UART инициализирован (TX=%d, RX=%d)", UART_MAIN_TXD, UART_MAIN_RXD);
}

void uart_send_command(const char *cmd) {
    char *cmd_copy = strdup(cmd);
    if (cmd_copy) {
        xQueueSend(tx_queue, &cmd_copy, pdMS_TO_TICKS(100));
    }
}

bool uart_request_state(void) {
    xSemaphoreTake(state_semaphore, 0);
    waiting_for_state = true;
    uart_send_command("GET_STATE");
    
    bool ok = (xSemaphoreTake(state_semaphore, pdMS_TO_TICKS(STATE_TIMEOUT_MS)) == pdTRUE);
    waiting_for_state = false;
    return ok;
}

static void uart_tx_task(void *pvParameters) {
    char *cmd;
    while (1) {
        if (xQueueReceive(tx_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            uart_write_bytes(UART_MAIN_PORT, cmd, strlen(cmd));
            uart_write_bytes(UART_MAIN_PORT, "\r\n", 2);
            ESP_LOGI(TAG, "📤 Отправлено: %s", cmd);
            free(cmd);
        }
    }
}

static void uart_rx_task(void *pvParameters) {
    uint8_t data[BUF_SIZE];
    char line[BUF_SIZE];
    int pos = 0;

    // Запускаем задачу отправки
    xTaskCreate(uart_tx_task, "uart_tx_task", 4096, NULL, 5, NULL);

    while (1) {
        int len = uart_read_bytes(UART_MAIN_PORT, data, sizeof(data) - 1, pdMS_TO_TICKS(50));
        for (int i = 0; i < len; i++) {
            if (data[i] == '\n' || data[i] == '\r') {
                if (pos > 0) {
                    line[pos] = '\0';
                    
                    if (strncmp(line, "STATE ", 6) == 0) {
                        uint32_t state;
                        if (sscanf(line + 6, "%" SCNx32, &state) == 1) {
                            state_manager_update(state);
                            if (waiting_for_state) {
                                xSemaphoreGive(state_semaphore);
                            }
                        }
                    }
                    else if (strcmp(line, "OK") == 0) {
                        ESP_LOGD(TAG, "✅ Команда выполнена");
                    }
                    else if (strcmp(line, "ERROR") == 0) {
                        ESP_LOGE(TAG, "❌ Ошибка выполнения");
                    }
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
    xTaskCreate(uart_rx_task, "uart_rx_task", 4096, NULL, 6, NULL);
}