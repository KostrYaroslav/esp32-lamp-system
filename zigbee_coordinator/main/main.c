#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "uart_handler.h"
#include "command_parser.h"

static const char *TAG = "ZIGBEE_GATEWAY";

void app_main(void) {
    // Инициализация NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Инициализация модулей
    uart_init();
    parser_init();

    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "🟢 Zigbee шлюз (ГОЛОВА) запущен");
    ESP_LOGI(TAG, "⚙️ Алгоритм: Запрос состояния → Расчет дельты → Команда");
    ESP_LOGI(TAG, "🚨 Аварийный режим: при таймауте команда шлется напрямую");
    ESP_LOGI(TAG, "💡 Введите HELP в монитор порта");
    ESP_LOGI(TAG, "═══════════════════════════════════════════");

    // Запуск задач
    parser_start_task();      // терминал (HELP, ON, OFF, STATUS...)
    uart_start_rx_task();     // приём STATE от Главного
}  