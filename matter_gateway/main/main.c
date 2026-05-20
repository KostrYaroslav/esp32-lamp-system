#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "uart_handler.h"

static const char *TAG = "MATTER_GATEWAY";

static bool lamp_state = false;

void matter_update_state(bool state) {
    lamp_state = state;
    ESP_LOGI(TAG, "💡 Лампа 1 теперь %s", state ? "ВКЛЮЧЕНА" : "ВЫКЛЮЧЕНА");
}

void matter_update_state_by_cmd(const char *cmd) {
    uint32_t state;
    if (sscanf(cmd, "STATE %" SCNx32, &state) == 1) {
        bool lamp1_state = (state >> 0) & 1;
        if (lamp1_state != lamp_state) {
            matter_update_state(lamp1_state);
        }
    } else {
        ESP_LOGD(TAG, "Неизвестная строка: %s", cmd);
    }
}

void matter_process_command(const char *cmd) {
    uart_send_command(cmd);
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    uart_init();
    uart_start_rx_task();

    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "🟢 Matter шлюз запущен (1 лампа)");
    ESP_LOGI(TAG, "💡 Лампа 1");
    ESP_LOGI(TAG, "═══════════════════════════════════════════");

    // Демо-тест: включить лампу 1 через 5 секунд, выключить через 10
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "🎯 ДЕМО: ON 1");
    matter_process_command("ON 1");
    
    vTaskDelay(pdMS_TO_TICKS(10000));
    ESP_LOGI(TAG, "🎯 ДЕМО: OFF 1");
    matter_process_command("OFF 1");
}