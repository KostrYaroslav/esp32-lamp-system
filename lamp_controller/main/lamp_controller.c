#include <stdio.h>
#include <string.h>
#include <inttypes.h>           // ДОБАВИТЬ для PRIX32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "driver/gpio.h"

static const char *TAG = "CONTROLLER";

// ==================== SN74HC595 ПИНЫ ====================
#define HC595_DATA_PIN   14
#define HC595_CLOCK_PIN  13
#define HC595_LATCH_PIN  12
#define HC595_OE_PIN     5

// ==================== ESP-NOW ====================
static uint8_t main_mac[] = {0x20, 0x6E, 0xF1, 0x13, 0x99, 0xE4};

// ==================== ПАРАМЕТРЫ ====================
#define PULSE_DURATION_MS 500     
#define QUEUE_SIZE        10

// ==================== ОЧЕРЕДЬ КОМАНД ====================
static QueueHandle_t mask_queue = NULL;
static uint32_t shift_reg_state = 0;

// ==================== SN74HC595 ФУНКЦИИ ====================

static void hc595_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << HC595_DATA_PIN) | 
                        (1ULL << HC595_CLOCK_PIN) |
                        (1ULL << HC595_LATCH_PIN) |
                        (1ULL << HC595_OE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    gpio_set_level(HC595_OE_PIN, 0);
    ESP_LOGI(TAG, "✅ SN74HC595 инициализирован (32 реле)");
}

static inline void hc595_write_byte(uint8_t data) {
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(HC595_DATA_PIN, (data >> i) & 1);
        gpio_set_level(HC595_CLOCK_PIN, 1);
        gpio_set_level(HC595_CLOCK_PIN, 0);
    }
}

static void hc595_update(void) {
    gpio_set_level(HC595_LATCH_PIN, 0);
    hc595_write_byte((shift_reg_state >> 24) & 0xFF);
    hc595_write_byte((shift_reg_state >> 16) & 0xFF);
    hc595_write_byte((shift_reg_state >> 8) & 0xFF);
    hc595_write_byte(shift_reg_state & 0xFF);
    gpio_set_level(HC595_LATCH_PIN, 1);
}

static void hc595_set_channels(uint32_t mask, bool state) {
    if (state) {
        shift_reg_state |= mask;
    } else {
        shift_reg_state &= ~mask;
    }
    hc595_update();
}

// ==================== ESP-NOW CALLBACK ====================

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    uint32_t mask = 0;
    
    if (len == 4) {
        memcpy(&mask, data, 4);
    } else if (len == 2) {
        uint16_t val;
        memcpy(&val, data, 2);
        mask = val;
    } else if (len == 1) {
        mask = data[0];
    } else {
        ESP_LOGW(TAG, "❌ Неверная длина данных: %d", len);
        return;
    }
    
    if (mask == 0) return;

    // Безопасная отправка в очередь из контекста прерывания WiFi
    if (xQueueSendFromISR(mask_queue, &mask, NULL) != pdPASS) {
        // ИСПРАВЛЕНО: добавлен PRIX32
        ESP_LOGW(TAG, "⚠️ Очередь переполнена! Пропущена маска: %08" PRIX32, mask);
    }
}

static void init_espnow(void) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));
    
    esp_now_peer_info_t peer = {
        .channel = 0,
        .ifidx = 0,           // ИСПРАВЛЕНО: 0 вместо WIFI_IF_STA
        .encrypt = false,
    };
    memcpy(peer.peer_addr, main_mac, 6);
    
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка добавления peer: %d", err);
    } else {
        ESP_LOGI(TAG, "✅ Peer Главного добавлен");
    }
}

// ==================== ОСНОВНОЙ ПОТОК ОБРАБОТКИ ====================

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    mask_queue = xQueueCreate(QUEUE_SIZE, sizeof(uint32_t));
    if (mask_queue == NULL) {
        ESP_LOGE(TAG, "❌ Не удалось создать очередь");
        return;
    }
    
    hc595_init();
    init_espnow();
    
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "📡 MAC: %02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "🟢 Управляющий запущен (32 реле, импульс %d мс)", PULSE_DURATION_MS);
    
    uint32_t active_mask = 0;

    while (1) {
        if (xQueueReceive(mask_queue, &active_mask, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "🔛 Включение маски: %08" PRIX32 " (%d реле)", 
                     active_mask, __builtin_popcount(active_mask));
            hc595_set_channels(active_mask, true);
            
            vTaskDelay(pdMS_TO_TICKS(PULSE_DURATION_MS));
            
            hc595_set_channels(active_mask, false);
            ESP_LOGI(TAG, "🔴 Выключение маски: %08" PRIX32, active_mask);
        }
    }
}