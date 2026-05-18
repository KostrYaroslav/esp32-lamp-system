#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "state_manager.h"
#include "uart_handler.h"

static const char *TAG = "COMMAND_PARSER";

#define RELAY_COUNT 32
#define UART_TERM_PORT UART_NUM_0
#define BUF_SIZE 256

typedef enum {
    CMD_MASK_SET,
    CMD_MASK_CLR,
    CMD_ON,
    CMD_OFF,
    CMD_ALL_ON,
    CMD_ALL_OFF,
    CMD_STATUS,
    CMD_HELP
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint32_t mask;
    int lamp_num;
} command_t;

static void execute_command(command_t *cmd) {
    uint32_t need_mask = 0;
    bool is_set = false;
    
    switch (cmd->type) {
        case CMD_MASK_SET:
            need_mask = cmd->mask;
            is_set = true;
            break;
        case CMD_MASK_CLR:
            need_mask = cmd->mask;
            is_set = false;
            break;
        case CMD_ON:
            need_mask = 1UL << (cmd->lamp_num - 1);
            is_set = true;
            break;
        case CMD_OFF:
            need_mask = 1UL << (cmd->lamp_num - 1);
            is_set = false;
            break;
        case CMD_ALL_ON:
            need_mask = 0xFFFFFFFF;
            is_set = true;
            break;
        case CMD_ALL_OFF:
            need_mask = 0xFFFFFFFF;
            is_set = false;
            break;
        default:
            return;
    }
    
    ESP_LOGI(TAG, "🔍 Запрашиваю состояние ламп перед командой...");
    
    if (uart_request_state()) {
        uint32_t diff = state_manager_calculate_diff(need_mask, is_set);
        if (diff != 0) {
            ESP_LOGI(TAG, "🎯 Нужно %s разницу: %08" PRIX32, 
                     is_set ? "включить" : "выключить", diff);
            state_manager_execute_command(diff, is_set);
        } else {
            ESP_LOGI(TAG, "✅ Все лампы уже %s", is_set ? "включены" : "выключены");
        }
    } else {
        ESP_LOGW(TAG, "⚠️ АВАРИЙНЫЙ РЕЖИМ: выполняю команду без проверки состояния");
        state_manager_execute_command(need_mask, is_set);
    }
}

static void parse_line(char *line) {
    command_t cmd = {0};
    uint32_t mask = 0;
    unsigned int lamp = 0;
    
    // Удаляем пробелы в конце
    int len = strlen(line);
    while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t' || line[len-1] == '\r' || line[len-1] == '\n')) {
        line[--len] = '\0';
    }
    if (len == 0) return;
    
    if (sscanf(line, "MASK_SET %" SCNx32, &mask) == 1) {
        cmd.type = CMD_MASK_SET;
        cmd.mask = mask;
    }
    else if (sscanf(line, "MASK_CLR %" SCNx32, &mask) == 1) {
        cmd.type = CMD_MASK_CLR;
        cmd.mask = mask;
    }
    else if (sscanf(line, "ON %u", &lamp) == 1) {
        if (lamp >= 1 && lamp <= RELAY_COUNT) {
            cmd.type = CMD_ON;
            cmd.lamp_num = lamp;
        } else {
            ESP_LOGW(TAG, "❌ Неверный номер лампы: %u (1-%d)", lamp, RELAY_COUNT);
            return;
        }
    }
    else if (sscanf(line, "OFF %u", &lamp) == 1) {
        if (lamp >= 1 && lamp <= RELAY_COUNT) {
            cmd.type = CMD_OFF;
            cmd.lamp_num = lamp;
        } else {
            ESP_LOGW(TAG, "❌ Неверный номер лампы: %u (1-%d)", lamp, RELAY_COUNT);
            return;
        }
    }
    else if (strcmp(line, "ALL_ON") == 0) {
        cmd.type = CMD_ALL_ON;
    }
    else if (strcmp(line, "ALL_OFF") == 0) {
        cmd.type = CMD_ALL_OFF;
    }
    else if (strcmp(line, "STATUS") == 0) {
        cmd.type = CMD_STATUS;
        if (uart_request_state()) {
            uint32_t state = state_manager_get_current();
            ESP_LOGI(TAG, "📊 Маска состояния: 0x%08" PRIX32, state);
            char active[128] = {0};
            int offset = 0;
            for (int i = 0; i < RELAY_COUNT; i++) {
                if (state & (1UL << i)) {
                    offset += snprintf(active + offset, sizeof(active) - offset, "%d ", i + 1);
                }
            }
            if (offset > 0) {
                ESP_LOGI(TAG, "💡 Включенные лампы: %s", active);
            } else {
                ESP_LOGI(TAG, "🌑 Все лампы выключены");
            }
        } else {
            ESP_LOGW(TAG, "⚠️ Не удалось получить состояние");
        }
        return;
    }
    else if (strcmp(line, "HELP") == 0) {
        ESP_LOGI(TAG, "═══════════════════════════════════════════");
        ESP_LOGI(TAG, "Доступные команды:");
        ESP_LOGI(TAG, "  MASK_SET <HEX>  - включить лампы по маске");
        ESP_LOGI(TAG, "  MASK_CLR <HEX>  - выключить лампы по маске");
        ESP_LOGI(TAG, "  ON <1-32>       - включить лампу");
        ESP_LOGI(TAG, "  OFF <1-32>      - выключить лампу");
        ESP_LOGI(TAG, "  ALL_ON          - включить все лампы");
        ESP_LOGI(TAG, "  ALL_OFF         - выключить все лампы");
        ESP_LOGI(TAG, "  STATUS          - показать состояние");
        ESP_LOGI(TAG, "  HELP            - справка");
        ESP_LOGI(TAG, "═══════════════════════════════════════════");
        return;
    }
    else {
        ESP_LOGW(TAG, "Неизвестная команда: %s (HELP)", line);
        return;
    }
    
    execute_command(&cmd);
}

static void terminal_task(void *pvParameters) {
    uint8_t data[BUF_SIZE];
    char line[BUF_SIZE];
    int pos = 0;

    ESP_LOGI(TAG, "⌨️ Терминал готов. HELP - список команд.");

    while (1) {
        int len = uart_read_bytes(UART_TERM_PORT, data, sizeof(data) - 1, pdMS_TO_TICKS(50));
        for (int i = 0; i < len; i++) {
            if (data[i] == '\n' || data[i] == '\r') {
                if (pos > 0) {
                    line[pos] = '\0';
                    parse_line(line);
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

void parser_init(void) {
    // Инициализация UART0 (терминал)
    if (!uart_is_driver_installed(UART_TERM_PORT)) {
        uart_config_t uart_config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        uart_param_config(UART_TERM_PORT, &uart_config);
        uart_driver_install(UART_TERM_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0);
    }
}

void parser_start_task(void) {
    xTaskCreate(terminal_task, "terminal_task", 4096, NULL, 4, NULL);
}