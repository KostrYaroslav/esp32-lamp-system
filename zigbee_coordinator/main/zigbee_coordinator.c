#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "ZIGBEE_GATEWAY";

// ==================== UART НАСТРОЙКИ ====================
#define UART_TERMINAL_PORT  UART_NUM_0
#define UART_MAIN_PORT      UART_NUM_1
#define UART_MAIN_TXD       4
#define UART_MAIN_RXD       5
#define BUF_SIZE            256

// ==================== ОТПРАВКА КОМАНД ГЛАВНОМУ ====================
static void send_to_main(const char *cmd) {
    if (cmd == NULL) return;
    
    uart_write_bytes(UART_MAIN_PORT, cmd, strlen(cmd));
    uart_write_bytes(UART_MAIN_PORT, "\r\n", 2);
    ESP_LOGI(TAG, "📤 Отправлено: %s", cmd);
}

// ==================== ПАРСИНГ КОМАНД ====================
static void parse_command(char *line) {
    char buffer[48];
    uint32_t mask = 0;
    unsigned int ch = 0;

    // Удаляем пробелы и символы перевода строки в конце
    int len = strlen(line);
    while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t' || line[len-1] == '\r' || line[len-1] == '\n')) {
        line[--len] = '\0';
    }
    
    if (len == 0) return;

    // MASK_SET
    if (sscanf(line, "MASK_SET %" SCNx32, &mask) == 1) {
        snprintf(buffer, sizeof(buffer), "MASK_SET %08" PRIX32, mask);
        send_to_main(buffer);
        return;
    }

    // MASK_CLR
    if (sscanf(line, "MASK_CLR %" SCNx32, &mask) == 1) {
        snprintf(buffer, sizeof(buffer), "MASK_CLR %08" PRIX32, mask);
        send_to_main(buffer);
        return;
    }

    // ON X
    if (sscanf(line, "ON %u", &ch) == 1) {
        if (ch >= 1 && ch <= 32) {
            mask = 1UL << (ch - 1);
            snprintf(buffer, sizeof(buffer), "MASK_SET %08" PRIX32, mask);
            send_to_main(buffer);
            ESP_LOGI(TAG, "🎯 Включить лампу %u", ch);
        } else {
            ESP_LOGW(TAG, "Неверный номер: %u (1-32)", ch);
        }
        return;
    }

    // OFF X
    if (sscanf(line, "OFF %u", &ch) == 1) {
        if (ch >= 1 && ch <= 32) {
            mask = 1UL << (ch - 1);
            snprintf(buffer, sizeof(buffer), "MASK_CLR %08" PRIX32, mask);
            send_to_main(buffer);
            ESP_LOGI(TAG, "🎯 Выключить лампу %u", ch);
        } else {
            ESP_LOGW(TAG, "Неверный номер: %u (1-32)", ch);
        }
        return;
    }

    // ALL_ON / ALL_OFF
    if (strcmp(line, "ALL_ON") == 0) {
        send_to_main("MASK_SET FFFFFFFF");
        ESP_LOGI(TAG, "🎯 ALL_ON");
        return;
    }
    
    if (strcmp(line, "ALL_OFF") == 0) {
        send_to_main("MASK_CLR FFFFFFFF");
        ESP_LOGI(TAG, "🎯 ALL_OFF");
        return;
    }

    // HELP
    if (strcmp(line, "HELP") == 0) {
        ESP_LOGI(TAG, "═══════════════════════════════════════════");
        ESP_LOGI(TAG, "Доступные команды:");
        ESP_LOGI(TAG, "  MASK_SET <HEX> - включить биты");
        ESP_LOGI(TAG, "  MASK_CLR <HEX> - выключить биты");
        ESP_LOGI(TAG, "  ON <1-32>      - включить лампу");
        ESP_LOGI(TAG, "  OFF <1-32>     - выключить лампу");
        ESP_LOGI(TAG, "  ALL_ON         - включить все");
        ESP_LOGI(TAG, "  ALL_OFF        - выключить все");
        ESP_LOGI(TAG, "  HELP           - справка");
        ESP_LOGI(TAG, "═══════════════════════════════════════════");
        return;
    }

    ESP_LOGW(TAG, "Неизвестная команда: %s", line);
}

// ==================== ПРИЁМ ОТВЕТОВ ОТ ГЛАВНОГО ====================
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
                    // Быстрая очистка пробельных символов на конце строки
                    while (pos > 0 && (line[pos-1] == ' ' || line[pos-1] == '\t')) {
                        line[--pos] = '\0';
                    }
                    if (pos > 0) {
                        ESP_LOGI(TAG, "📥 Ответ: %s", line);
                    }
                    pos = 0;
                }
            } else {
                if (pos < sizeof(line) - 1) {
                    line[pos++] = data[i];
                } else {
                    ESP_LOGW(TAG, "Превышен лимит буфера UART_MAIN, сброс строки");
                    pos = 0; 
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ==================== ТЕРМИНАЛЬНАЯ ЗАДАЧА ====================
static void terminal_task(void *pvParameters) {
    uint8_t data[BUF_SIZE];
    char line[BUF_SIZE];
    int pos = 0;

    ESP_LOGI(TAG, "⌨️ Терминал готов. HELP - список команд.");

    while (1) {
        // Оптимизированное пачечное чтение вместо побайтового
        int len = uart_read_bytes(UART_TERMINAL_PORT, data, sizeof(data) - 1, pdMS_TO_TICKS(50));
        
        for (int i = 0; i < len; i++) {
            if (data[i] == '\n' || data[i] == '\r') {
                if (pos > 0) {
                    line[pos] = '\0';
                    parse_command(line);
                    pos = 0;
                }
            } else {
                if (pos < sizeof(line) - 1) {
                    line[pos++] = data[i];
                } else {
                    ESP_LOGW(TAG, "Превышен лимит буфера терминала, сброс строки");
                    pos = 0;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ==================== ИНИЦИАЛИЗАЦИЯ UART ====================
static void init_uart(void) {
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    ESP_ERROR_CHECK(uart_param_config(UART_MAIN_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_MAIN_PORT, UART_MAIN_TXD, UART_MAIN_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_MAIN_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));
    
    // Инициализация драйвера для UART0 (терминал), если планируется чтение через API
    if (!uart_is_driver_installed(UART_TERMINAL_PORT)) {
        ESP_ERROR_CHECK(uart_driver_install(UART_TERMINAL_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));
    }
    
    ESP_LOGI(TAG, "✅ UART интерфейсы успешно настроены");
}

// ==================== MAIN ====================
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    init_uart();

    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "🟢 Zigbee шлюз запущен");
    ESP_LOGI(TAG, "📡 UART: TX=%d, RX=%d", UART_MAIN_TXD, UART_MAIN_RXD);
    ESP_LOGI(TAG, "💡 Введите HELP");
    ESP_LOGI(TAG, "═══════════════════════════════════════════");

    xTaskCreate(terminal_task, "terminal_task", 4096, NULL, 5, NULL);
    xTaskCreate(uart_rx_task, "uart_rx_task", 4096, NULL, 5, NULL);
}
