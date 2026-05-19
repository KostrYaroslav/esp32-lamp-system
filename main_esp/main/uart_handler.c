#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "uart_handler.h"
#include "command_parser.h"
#include "state_manager.h"

static const char *TAG = "UART";
#define UART_MATTER_PORT UART_NUM_1
#define UART_MATTER_TXD 4
#define UART_MATTER_RXD 5
#define UART_TERM_PORT UART_NUM_0
#define BUF_SIZE 256

// Мьютекс для разделения доступа к менеджеру состояний между задачами
static SemaphoreHandle_t cmd_mutex = NULL;

void uart_init(void) {
    cmd_mutex = xSemaphoreCreateMutex();
    if (cmd_mutex == NULL) {
        ESP_LOGE(TAG, "❌ Ошибка мьютекса UART команд");
    }

    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_MATTER_PORT, &cfg);
    uart_set_pin(UART_MATTER_PORT, UART_MATTER_TXD, UART_MATTER_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_MATTER_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0);
    
    if (!uart_is_driver_installed(UART_TERM_PORT)) {
        uart_driver_install(UART_TERM_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0);
    }
    ESP_LOGI(TAG, "✅ Драйверы UART настроены");
}

void uart_send_to_matter(const char *msg) {
    uart_write_bytes(UART_MATTER_PORT, msg, strlen(msg));
    uart_write_bytes(UART_MATTER_PORT, "\r\n", 2);
}

static void terminal_task(void *pv) {
    uint8_t data[BUF_SIZE];
    char line[BUF_SIZE];
    int pos = 0;
    ESP_LOGI(TAG, "⌨️ Консоль отладки активна.");
    
    while (1) {
        int len = uart_read_bytes(UART_TERM_PORT, data, sizeof(data) - 1, pdMS_TO_TICKS(50));
        for (int i = 0; i < len; i++) {
            if (data[i] == '\n' || data[i] == '\r') {
                if (pos > 0) {
                    line[pos] = '\0';
                    parsed_cmd_t cmd = parse_command(line);
                    
                    if (cmd.type == CMD_HELP) {
                        ESP_LOGI(TAG, "Доступно: ON X, OFF X, ALL_ON, ALL_OFF, STATUS, MASK_SET X, MASK_CLR X");
                    } else if (cmd.type != CMD_UNKNOWN) {
                        if (xSemaphoreTake(cmd_mutex, portMAX_DELAY) == pdTRUE) {
                            state_manager_process_command(cmd);
                            xSemaphoreGive(cmd_mutex);
                        }
                    } else {
                        ESP_LOGW(TAG, "Команда неизвестна: %s", line);
                    }
                    pos = 0;
                }
            } else {
                if (pos < sizeof(line) - 1) {
                    line[pos++] = data[i];
                } else {
                    ESP_LOGE(TAG, "Переполнение буфера терминала, сброс!");
                    pos = 0;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void matter_rx_task(void *pv) {
    uint8_t data[BUF_SIZE];
    char line[BUF_SIZE];
    int pos = 0;
    
    while (1) {
        int len = uart_read_bytes(UART_MATTER_PORT, data, sizeof(data) - 1, pdMS_TO_TICKS(50));
        for (int i = 0; i < len; i++) {
            if (data[i] == '\n' || data[i] == '\r') {
                if (pos > 0) {
                    line[pos] = '\0';
                    parsed_cmd_t cmd = parse_command(line);
                    if (cmd.type != CMD_UNKNOWN) {
                        if (xSemaphoreTake(cmd_mutex, portMAX_DELAY) == pdTRUE) {
                            state_manager_process_command(cmd);
                            xSemaphoreGive(cmd_mutex);
                        }
                    }
                    pos = 0;
                }
            } else {
                if (pos < sizeof(line) - 1) {
                    line[pos++] = data[i];
                } else {
                    ESP_LOGE(TAG, "Переполнение буфера Matter, сброс!");
                    pos = 0;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void uart_start_terminal_task(void) {
    xTaskCreate(terminal_task, "terminal_task", 4096, NULL, 4, NULL);
}

void uart_start_matter_rx_task(void) {
    xTaskCreate(matter_rx_task, "matter_rx_task", 4096, NULL, 5, NULL);
}