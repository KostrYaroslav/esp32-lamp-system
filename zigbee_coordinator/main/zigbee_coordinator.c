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
#define UART_TERMINAL_PORT  UART_NUM_0      // USB (монитор порта)
#define UART_MAIN_PORT      UART_NUM_1      // связь с Главным
#define UART_MAIN_TXD       4               // GPIO4 (TX к Главному)
#define UART_MAIN_RXD       5               // GPIO5 (RX от Главного)
#define BUF_SIZE            256

// ==================== ОТПРАВКА КОМАНД ГЛАВНОМУ ====================
static void send_to_main(const char *cmd) {
    if (cmd == NULL) return;
    
    uart_write_bytes(UART_MAIN_PORT, cmd, strlen(cmd));
    uart_write_bytes(UART_MAIN_PORT, "\r\n", 2);
    ESP_LOGI(TAG, "📤 Отправлено: %s", cmd);
}

// ==================== ПАРСИНГ КОМАНД ТЕРМИНАЛА ====================
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

    // 1. MASK_SET - включить по маске
    if (sscanf(line, "MASK_SET %" SCNx32, &mask) == 1) {
        snprintf(buffer, sizeof(buffer), "MASK_SET %08" PRIX32, mask);
        send_to_main(buffer);
        return;
    }

    // 2. MASK_CLR - выключить по маске
    if (sscanf(line, "MASK_CLR %" SCNx32, &mask) == 1) {
        snprintf(buffer, sizeof(buffer), "MASK_CLR %08" PRIX32, mask);
        send_to_main(buffer);
        return;
    }

    // 3. ON <номер> - включить лампу
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

    // 4. OFF <номер> - выключить лампу
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

    // 5. ALL_ON - включить все
    if (strcmp(line, "ALL_ON") == 0) {
        send_to_main("MASK_SET FFFFFFFF");
        ESP_LOGI(TAG, "🎯 ALL_ON");
        return;
    }
    
    // 6. ALL_OFF - выключить все
    if (strcmp(line, "ALL_OFF") == 0) {
        send_to_main("MASK_CLR FFFFFFFF");
        ESP_LOGI(TAG, "🎯 ALL_OFF");
        return;
    }

    // 7. STATUS - запросить состояние ламп
    if (strcmp(line, "STATUS") == 0) {
        send_to_main("GET_STATE");
        ESP_LOGI(TAG, "🔍 Запрос состояния ламп");
        return;
    }

    // 8. HELP - справка
    if (strcmp(line, "HELP") == 0) {
        ESP_LOGI(TAG, "═══════════════════════════════════════════");
        ESP_LOGI(TAG, "Доступные команды:");
        ESP_LOGI(TAG, "  MASK_SET <HEX> - включить биты по маске");
        ESP_LOGI(TAG, "  MASK_CLR <HEX> - выключить биты по маске");
        ESP_LOGI(TAG, "  ON <1-32>      - включить лампу");
        ESP_LOGI(TAG, "  OFF <1-32>     - выключить лампу");
        ESP_LOGI(TAG, "  ALL_ON         - включить все 32 лампы");
        ESP_LOGI(TAG, "  ALL_OFF        - выключить все 32 лампы");
        ESP_LOGI(TAG, "  STATUS         - запросить состояние ламп");
        ESP_LOGI(TAG, "  HELP           - показать справку");
        ESP_LOGI(TAG, "═══════════════════════════════════════════");
        return;
    }

    // 9. Неизвестная команда
    if (strlen(line) > 0) {
        ESP_LOGW(TAG, "Неизвестная команда: %s", line);
    }
}

// ==================== ПРИЁМ ОТВЕТОВ И СОСТОЯНИЙ ОТ ГЛАВНОГО ====================
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
                    
                    // Удаляем пробелы в конце перед обработкой
                    while (pos > 0 && (line[pos-1] == ' ' || line[pos-1] == '\t')) {
                        line[--pos] = '\0';
                    }
                    
                    // Парсинг STATE от Главного (состояние ламп)
                    if (strncmp(line, "STATE ", 6) == 0) {
                        uint32_t state;
                        if (sscanf(line + 6, "%" SCNx32, &state) == 1) {
                            ESP_LOGI(TAG, "═══════════════════════════════════════════");
                            ESP_LOGI(TAG, "📊 СОСТОЯНИЕ ЛАМП: %08" PRIX32, state);
                            for (int k = 0; k < 16; k++) {
                                if (state & (1UL << k)) {
                                    ESP_LOGI(TAG, "   Лампа %2d: 🔆 ВКЛ", k + 1);
                                } else {
                                    ESP_LOGI(TAG, "   Лампа %2d: ○ ВЫКЛ", k + 1);
                                }
                            }
                            if (state >> 16) {
                                ESP_LOGI(TAG, "   ... и старшие 16 ламп");
                            }
                            ESP_LOGI(TAG, "═══════════════════════════════════════════");
                        }
                    }
                    else {
                        ESP_LOGI(TAG, "📥 Ответ: %s", line);
                    }
                    pos = 0;
                }
            } else {
                if (pos < sizeof(line) - 1) {
                    line[pos++] = data[i];
                } else {
                    ESP_LOGW(TAG, "Буфер RX главного переполнен, сброс");
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
                    ESP_LOGW(TAG, "Буфер терминала переполнен, сброс строки");
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
    
    if (!uart_is_driver_installed(UART_TERMINAL_PORT)) {
        ESP_ERROR_CHECK(uart_driver_install(UART_TERMINAL_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));
    }
    
    ESP_LOGI(TAG, "✅ UART инициализирован (TX=%d, RX=%d)", UART_MAIN_TXD, UART_MAIN_RXD);
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
    ESP_LOGI(TAG, "💡 Введите HELP для списка команд");
    ESP_LOGI(TAG, "═══════════════════════════════════════════");

    xTaskCreate(terminal_task, "terminal_task", 4096, NULL, 5, NULL);
    xTaskCreate(uart_rx_task, "uart_rx_task", 4096, NULL, 5, NULL);
}
