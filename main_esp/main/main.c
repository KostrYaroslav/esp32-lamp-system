#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "uart_handler.h"
#include "espnow_handler.h"
#include "state_manager.h"

static const char *TAG = "MAIN_ESP";

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    uart_init();
    espnow_init();
    state_manager_init();

    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "🟢 Главный (ESP32-C6) запущен в режиме УМНЫЙ");
    ESP_LOGI(TAG, "⚙️ Функции: парсинг команд, анализ, аварийный режим");
    ESP_LOGI(TAG, "═══════════════════════════════════════════");

    uart_start_terminal_task();
    uart_start_matter_rx_task();
}