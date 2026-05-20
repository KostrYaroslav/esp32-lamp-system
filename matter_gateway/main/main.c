#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "uart_handler.h"

// Заглушки для Matter (пока без реального Matter стека)
static bool lamp_state = false;

void matter_update_state(bool state) {
    lamp_state = state;
    ESP_LOGI("MATTER", "Лампа 1 теперь %s", state ? "ВКЛЮЧЕНА" : "ВЫКЛЮЧЕНА");
}

void matter_process_command(const char *cmd) {
    // Отправляем команду Главному по UART
    uart_send_command(cmd);
}

static const char *TAG = "MATTER_GATEWAY";

void app_main(void) {
    // Инициализация NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Инициализация сетевых интерфейсов
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Инициализация UART
    uart_init();
    uart_start_rx_task();

    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "🟢 Matter шлюз запущен (1 лампа)");
    ESP_LOGI(TAG, "💡 Лампа 1");
    ESP_LOGI(TAG, "═══════════════════════════════════════════");

    // ДЕМО: эмуляция команд (потом заменим на реальный Matter)
    // В реальности команды будет присылать Matter стек
    
    // Демо: включить лампу 1
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "🎯 ДЕМО: включаю лампу 1");
    matter_process_command("ON 1");
    
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "🎯 ДЕМО: выключаю лампу 1");
    matter_process_command("OFF 1");
}