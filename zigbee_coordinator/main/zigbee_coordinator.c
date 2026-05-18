#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "ZIGBEE_GATEWAY";

// ==================== UART НАСТРОЙКИ ====================
#define UART_MAIN_PORT      UART_NUM_1      // Связь с Главным контроллером
#define UART_MAIN_TXD       4               // GPIO4 (TX к Главному)
#define UART_MAIN_RXD       5               // GPIO5 (RX от Главного)

#define UART_TERM_PORT      UART_NUM_0      // Порт терминала/компьютера
#define BUF_SIZE            256

// ==================== ПАРАМЕТРЫ ====================
#define RELAY_COUNT         32
#define STATE_TIMEOUT_MS    500             // Таймаут ожидания ответа от Главного

// ==================== СТРУКТУРЫ ====================
typedef enum {
    ACTION_NONE,
    ACTION_SET,
    ACTION_CLR
} action_t;

typedef struct {
    action_t action;
    uint32_t mask;
} pending_command_t;

static QueueHandle_t command_queue = NULL;
static SemaphoreHandle_t state_semaphore = NULL;
static uint32_t current_lamp_state = 0;
static bool has_state = false;
static bool waiting_for_state = false;

// ==================== UART ОТПРАВКА ГЛАВНОМУ ====================
static void send_to_main(const char *cmd) {
    if (cmd == NULL) return;
    uart_write_bytes(UART_MAIN_PORT, cmd, strlen(cmd));
    uart_write_bytes(UART_MAIN_PORT, "\r\n", 2);
    ESP_LOGI(TAG, "📤 Отправлено Главному: %s", cmd);
}

// ==================== ЗАПРОС СОСТОЯНИЯ ====================
static bool request_state(void) {
    // Сбрасываем семафор перед новым запросом
    xSemaphoreTake(state_semaphore, 0);
    
    waiting_for_state = true;
    send_to_main("GET_STATE");
    
    // Ждём ответ с таймаутом
    if (xSemaphoreTake(state_semaphore, pdMS_TO_TICKS(STATE_TIMEOUT_MS)) == pdTRUE) {
        waiting_for_state = false;
        return true;
    }
    
    waiting_for_state = false;
    ESP_LOGW(TAG, "⏱️ Таймаут: ответ от Главного не получен");
    return false;
}

// ==================== ВЫПОЛНЕНИЕ КОМАНДЫ ====================
static void execute_command(action_t action, uint32_t mask) {
    char buffer[48];
    
    if (mask == 0) {
        ESP_LOGW(TAG, "⚠️ Пустая маска, команда не выполнена");
        return;
    }
    
    if (action == ACTION_SET) {
        snprintf(buffer, sizeof(buffer), "MASK_SET %08" PRIX32, mask);
        ESP_LOGI(TAG, "💡 Включение ламп по маске: %08" PRIX32, mask);
    } else {
        snprintf(buffer, sizeof(buffer), "MASK_CLR %08" PRIX32, mask);
        ESP_LOGI(TAG, "💡 Выключение ламп по маске: %08" PRIX32, mask);
    }
    
    send_to_main(buffer);
}

// ==================== ОБРАБОТКА ОТВЕТА STATE ====================
static void process_state(const char *state_str) {
    uint32_t new_state;
    if (sscanf(state_str, "%" SCNx32, &new_state) == 1) {
        current_lamp_state = new_state;
        has_state = true;
        ESP_LOGI(TAG, "📊 Состояние ламп обновлено: %08" PRIX32, new_state);
        
        // Если кто-то ждал ответа — освобождаем семафор
        if (waiting_for_state) {
            xSemaphoreGive(state_semaphore);
        }
    } else {
        ESP_LOGE(TAG, "❌ Ошибка парсинга STATE: %s", state_str);
    }
}

// ==================== ОБРАБОТКА NO_STATE ====================
static void process_no_state(void) {
    ESP_LOGW(TAG, "⚠️ Главный не получил состояние от Смотрящего");
    if (waiting_for_state) {
        xSemaphoreGive(state_semaphore);  // Разблокируем таску, логика сама обработает флаг
    }
}

// ==================== UART ПРИЁМ ОТ ГЛАВНОГО ====================
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
                    
                    // Удаляем пробелы в конце строки
                    while (pos > 0 && (line[pos-1] == ' ' || line[pos-1] == '\t')) {
                        line[--pos] = '\0';
                    }
                    
                    if (strncmp(line, "STATE ", 6) == 0) {
                        process_state(line + 6);
                    }
                    else if (strcmp(line, "NO_STATE") == 0) {
                        process_no_state();
                    }
                    else if (strcmp(line, "OK") == 0) {
                        ESP_LOGD(TAG, "✅ Главный подтвердил выполнение команды");
                    }
                    else if (strcmp(line, "ERROR") == 0) {
                        ESP_LOGE(TAG, "❌ Главный сообщил об ошибке");
                    }
                    else {
                        ESP_LOGD(TAG, "📥 Получено от Главного: %s", line);
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

// ==================== ОБРАБОТКА ВНУТРЕННИХ КОМАНД ====================
static void command_task(void *pvParameters) {
    pending_command_t cmd;
    
    while (1) {
        if (xQueueReceive(command_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "🔍 Запрашиваю состояние ламп перед командой...");
            
            if (request_state()) {
                // Состояние получено, анализируем изменения
                uint32_t need_mask = cmd.mask;
                uint32_t current = current_lamp_state;
                
                if (cmd.action == ACTION_SET) {
                    // Включаем только те, которые сейчас выключены
                    uint32_t to_enable = need_mask & (~current);
                    if (to_enable != 0) {
                        ESP_LOGI(TAG, "🎯 Нужно включить разницу: %08" PRIX32, to_enable);
                        execute_command(ACTION_SET, to_enable);
                    } else {
                        ESP_LOGI(TAG, "✅ Все запрошенные лампы уже включены");
                    }
                } else {
                    // Выключаем только те, которые сейчас включены
                    uint32_t to_disable = need_mask & current;
                    if (to_disable != 0) {
                        ESP_LOGI(TAG, "🎯 Нужно выключить разницу: %08" PRIX32, to_disable);
                        execute_command(ACTION_CLR, to_disable);
                    } else {
                        ESP_LOGI(TAG, "✅ Все запрошенные лампы уже выключены");
                    }
                }
            } else {
                // АВАРИЙНЫЙ РЕЖИМ: выполняем команду без проверки, если связь оборвалась
                ESP_LOGW(TAG, "⚠️ АВАРИЙНЫЙ РЕЖИМ: выполняю команду без проверки состояния");
                execute_command(cmd.action, cmd.mask);
            }
        }
    }
}

// ==================== ПАРСИНГ КОМАНД ТЕРМИНАЛА ====================
static void parse_terminal_command(char *line) {
    pending_command_t cmd;
    uint32_t mask = 0;
    unsigned int ch = 0;
    
    // Удаляем концевые символы переноса и пробелы
    int len = strlen(line);
    while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t' || line[len-1] == '\r' || line[len-1] == '\n')) {
        line[--len] = '\0';
    }
    
    if (len == 0) return;
    
    // MASK_SET <HEX>
    if (sscanf(line, "MASK_SET %" SCNx32, &mask) == 1) {
        cmd.action = ACTION_SET;
        cmd.mask = mask;
        xQueueSend(command_queue, &cmd, pdMS_TO_TICKS(100));
        return;
    }
    
    // MASK_CLR <HEX>
    if (sscanf(line, "MASK_CLR %" SCNx32, &mask) == 1) {
        cmd.action = ACTION_CLR;
        cmd.mask = mask;
        xQueueSend(command_queue, &cmd, pdMS_TO_TICKS(100));
        return;
    }
    
    // ON X
    if (sscanf(line, "ON %u", &ch) == 1) {
        if (ch >= 1 && ch <= RELAY_COUNT) {
            cmd.action = ACTION_SET;
            cmd.mask = 1UL << (ch - 1);
            xQueueSend(command_queue, &cmd, pdMS_TO_TICKS(100));
            ESP_LOGI(TAG, "🎯 Команда из консоли: включить лампу %u", ch);
        } else {
            ESP_LOGW(TAG, "❌ Неверный номер лампы: %u (Допустимо: 1-%d)", ch, RELAY_COUNT);
        }
        return;
    }
    
    // OFF X
    if (sscanf(line, "OFF %u", &ch) == 1) {
        if (ch >= 1 && ch <= RELAY_COUNT) {
            cmd.action = ACTION_CLR;
            cmd.mask = 1UL << (ch - 1);
            xQueueSend(command_queue, &cmd, pdMS_TO_TICKS(100));
            ESP_LOGI(TAG, "🎯 Команда из консоли: выключить лампу %u", ch);
        } else {
            ESP_LOGW(TAG, "❌ Неверный номер лампы: %u (Допустимо: 1-%d)", ch, RELAY_COUNT);
        }
        return;
    }
    
    // ALL_ON
    if (strcmp(line, "ALL_ON") == 0) {
        cmd.action = ACTION_SET;
        cmd.mask = 0xFFFFFFFF;
        xQueueSend(command_queue, &cmd, pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "🎯 Команда из консоли: включить ВСЕ лампы");
        return;
    }
    
    // ALL_OFF
    if (strcmp(line, "ALL_OFF") == 0) {
        cmd.action = ACTION_CLR;
        cmd.mask = 0xFFFFFFFF;
        xQueueSend(command_queue, &cmd, pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "🎯 Команда из консоли: выключить ВСЕ лампы");
        return;
    }
    
    // STATUS (Оптимизированный вывод в одну строку)
    if (strcmp(line, "STATUS") == 0) {
        ESP_LOGI(TAG, "🔍 Запрос текущего статуса...");
        if (request_state()) {
            ESP_LOGI(TAG, "📊 Маска состояния: 0x%08" PRIX32, current_lamp_state);
            
            char active_lamps_buf[128] = {0};
            int offset = 0;
            for (int i = 0; i < RELAY_COUNT; i++) {
                if (current_lamp_state & (1UL << i)) {
                    offset += snprintf(active_lamps_buf + offset, sizeof(active_lamps_buf) - offset, "%d ", i + 1);
                }
            }
            if (offset > 0) {
                ESP_LOGI(TAG, "💡 Включенные лампы: %s", active_lamps_buf);
            } else {
                ESP_LOGI(TAG, "🌑 Все лампы выключены");
            }
        } else {
            ESP_LOGW(TAG, "⚠️ Не удалось получить состояние от Главного");
        }
        return;
    }

    // SPONTANEOUS (Для теста — эмуляция спонтанного изменения от Смотрящего)
    if (strcmp(line, "SPONTANEOUS") == 0) {
        uint32_t new_state = 0xAAAAAAAA;
        ESP_LOGI(TAG, "🔄 Тестовое спонтанное состояние: %08" PRIX32, new_state);
        current_lamp_state = new_state;
        has_state = true;
        return;
    }
    
    // HELP
    if (strcmp(line, "HELP") == 0) {
        ESP_LOGI(TAG, "═══════════════════════════════════════════");
        ESP_LOGI(TAG, "Доступные команды:");
        ESP_LOGI(TAG, "  MASK_SET <HEX>  - включить лампы по маске");
        ESP_LOGI(TAG, "  MASK_CLR <HEX>  - выключить лампы по маске");
        ESP_LOGI(TAG, "  ON <1-32>       - включить одну лампу");
        ESP_LOGI(TAG, "  OFF <1-32>      - выключить одну лампу");
        ESP_LOGI(TAG, "  ALL_ON          - включить все лампы");
        ESP_LOGI(TAG, "  ALL_OFF         - выключить все лампы");
        ESP_LOGI(TAG, "  STATUS          - показать состояние ламп");
        ESP_LOGI(TAG, "  SPONTANEOUS     - тест спонтанного состояния");
        ESP_LOGI(TAG, "  HELP            - эта справка");
        ESP_LOGI(TAG, "═══════════════════════════════════════════");
        return;
    }
    
    ESP_LOGW(TAG, "Неизвестная команда: %s (Введите HELP)", line);
}

// ==================== ТЕРМИНАЛЬНАЯ ЗАДАЧА ====================
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
                    parse_terminal_command(line);
                    pos = 0;
                }
            } else {
                if (pos < sizeof(line) - 1) {
                    line[pos++] = data[i];
                } else {
                    ESP_LOGW(TAG, "⚠️ Буфер ввода терминала переполнен, сброс");
                    pos = 0;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ==================== ИНИЦИАЛИЗАЦИЯ UART ПОРТОВ ====================
static void init_uart(void) {
    // 1. Настройка UART1 (связь с Главным контроллером)
    uart_config_t uart_config_main = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    ESP_ERROR_CHECK(uart_param_config(UART_MAIN_PORT, &uart_config_main));
    ESP_ERROR_CHECK(uart_set_pin(UART_MAIN_PORT, UART_MAIN_TXD, UART_MAIN_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_MAIN_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));
    
    // 2. Безопасная настройка UART0 (Терминал/Логи через USB)
    if (!uart_is_driver_installed(UART_TERM_PORT)) {
        uart_config_t uart_config_term = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        ESP_ERROR_CHECK(uart_param_config(UART_TERM_PORT, &uart_config_term));
        ESP_ERROR_CHECK(uart_driver_install(UART_TERM_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));
    }
    
    ESP_LOGI(TAG, "✅ Железо инициализировано. UART1 (Главный): TX=%d, RX=%d", UART_MAIN_TXD, UART_MAIN_RXD);
}

// ==================== ТОЧКА ВХОДА MAIN ====================
void app_main(void) {
    // Инициализация NVS-памяти для системных нужд ESP32
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Создание очереди межзадачного взаимодействия
    command_queue = xQueueCreate(10, sizeof(pending_command_t));
    if (command_queue == NULL) {
        ESP_LOGE(TAG, "❌ Критическая ошибка создания очереди команд");
        return;
    }
    
    // Создание бинарного семафора синхронизации STATE
    state_semaphore = xSemaphoreCreateBinary();
    if (state_semaphore == NULL) {
        ESP_LOGE(TAG, "❌ Критическая ошибка создания семафора");
        return;
    }

    // Инициализация драйверов UART
    init_uart();

    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "🟢 Zigbee шлюз (ГОЛОВА) успешно запущен");
    ESP_LOGI(TAG, "⚙️ Алгоритм: Запрос состояния → Расчет дельты → Команда");
    ESP_LOGI(TAG, "🚨 Авария: при таймауте команда шлется напрямую");
    ESP_LOGI(TAG, "💡 Для управления введите HELP в монитор порта");
    ESP_LOGI(TAG, "═══════════════════════════════════════════");

    // Создание параллельных задач FreeRTOS на обоих ядрах (tskNO_AFFINITY)
    xTaskCreatePinnedToCore(uart_rx_task,   "uart_rx_task",   4096, NULL, 6, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(command_task,   "command_task",   4096, NULL, 5, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(terminal_task,  "terminal_task",  4096, NULL, 4, NULL, tskNO_AFFINITY);
}
