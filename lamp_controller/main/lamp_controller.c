#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
// MAC Главного (ESP32-C6 №2)
static uint8_t main_mac[] = {0x20, 0x6E, 0xF1, 0x13, 0x99, 0xE4};

// ==================== ПАРАМЕТРЫ ====================
#define RELAY_COUNT       32      // 32 реле (0..31)
#define PULSE_DURATION_MS 500     // 500 мс

// ==================== СОСТОЯНИЕ ====================
static uint32_t shift_reg_state = 0;
static bool pulse_active = false;
static uint64_t pulse_end_time_us = 0;

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

static void hc595_write_byte(uint8_t data) {
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

// ==================== ИМПУЛЬСЫ ====================

static uint32_t current_mask = 0;

static void start_pulse(uint32_t mask) {
    if (pulse_active) {
        ESP_LOGW(TAG, "⚠️ Импульс уже активен, пропуск маски: %08X", mask);
        return;
    }
    
    current_mask = mask;
    hc595_set_channels(mask, true);  // включаем все реле из маски
    pulse_active = true;
    pulse_end_time_us = esp_timer_get_time() + (PULSE_DURATION_MS * 1000);
    ESP_LOGI(TAG, "🔛 Импульс на маску: %08X (%d мс, %d реле)", 
             mask, PULSE_DURATION_MS, __builtin_popcount(mask));
}

static void check_pulse_timeout(void) {
    if (pulse_active && esp_timer_get_time() >= pulse_end_time_us) {
        hc595_set_channels(current_mask, false);  // выключаем все реле
        pulse_active = false;
        ESP_LOGI(TAG, "🔴 Импульс завершён (маска: %08X)", current_mask);
        current_mask = 0;
    }
}

// ==================== ESP-NOW ====================

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    uint32_t mask = 0;
    
    if (len == 4) {
        mask = *(uint32_t*)data;
    } else if (len == 2) {
        mask = *(uint16_t*)data;
    } else if (len == 1) {
        mask = data[0];
    } else {
        ESP_LOGW(TAG, "Неверная длина: %d", len);
        return;
    }
    
    // Ограничиваем только 32 битами
    mask &= 0xFFFFFFFF;
    
    if (mask == 0) {
        ESP_LOGI(TAG, "📨 Получена пустая маска, игнорируем");
        return;
    }
    
    ESP_LOGI(TAG, "📨 Получена маска: %08X (%d реле)", mask, __builtin_popcount(mask));
    start_pulse(mask);
}

static void init_espnow(void) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));
    
    esp_now_peer_info_t peer = {
        .peer_addr = {main_mac[0], main_mac[1], main_mac[2],
                      main_mac[3], main_mac[4], main_mac[5]},
        .channel = 0,
        .ifidx = 0,
        .encrypt = false,
    };
    
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка добавления peer: %d", err);
    } else {
        ESP_LOGI(TAG, "Peer Главного добавлен");
    }
}

// ==================== MAIN ====================

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    hc595_init();
    init_espnow();
    
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "🟢 Управляющий запущен (32 реле, импульс %d мс, одновременный режим)", PULSE_DURATION_MS);
    ESP_LOGI(TAG, "📡 MAC Управляющего: %02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "⏳ Ожидание команд от Главного...");
    
    while (1) {
        check_pulse_timeout();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}