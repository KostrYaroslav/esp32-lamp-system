#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"

static const char *TAG = "MONITOR";

// TODO: ЗАМЕНИТЕ на реальный MAC Главного (из лога main_esp)
static uint8_t main_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Состояние 15 ламп (бит 0 = лампа 1, бит 14 = лампа 15)
static uint16_t lamp_state = 0x5555;

static esp_timer_handle_t status_timer = NULL;

// ИСПРАВЛЕНО: правильная сигнатура для ESP-IDF v6.x
static void send_status(void *arg) {
    esp_err_t ret = esp_now_send(main_mac, (uint8_t*)&lamp_state, sizeof(lamp_state));
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "📤 Статус отправлен: %04X", lamp_state);
    } else {
        ESP_LOGE(TAG, "❌ Ошибка отправки: %d", ret);
    }
    
    // Имитация изменения состояния
    lamp_state = (lamp_state << 1) | ((lamp_state >> 14) & 1);
}

// ИСПРАВЛЕНО: правильная сигнатура callback для ESP-IDF v6.x
static void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGD(TAG, "✅ Доставлено");
    } else {
        ESP_LOGW(TAG, "⚠️ Не доставлено");
    }
}

// Инициализация ESP-NOW
static void init_espnow(void) {
    // Wi-Fi в режиме STA
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    
    // ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    
    // ИСПРАВЛЕНО: убрано ESP_IF_WIFI_STA (используем 0)



// Добавляем peer (Главный)
    esp_now_peer_info_t peer = {
        .peer_addr = {main_mac[0], main_mac[1], main_mac[2], 
                        main_mac[3], main_mac[4], main_mac[5]},
        .channel = 0,
        .ifidx = 0,              // ← ЗДЕСЬ БЫЛО ESP_IF_WIFI_STA
        .encrypt = false,
    };
    
    // Альтернатива, если ESP_IF_WIFI_STA не определён:
    // peer.ifidx = 0;  // ESP_IF_WIFI_STA обычно равен 0
    
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка добавления peer: %d", err);
    } else {
        ESP_LOGI(TAG, "Peer добавлен");
    }
}

// ИСПРАВЛЕНО: колбэк теперь принимает void *arg
static void init_timer(void) {
    esp_timer_create_args_t timer_args = {
        .callback = &send_status,
        .name = "status_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &status_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(status_timer, 30000000));  // 30 сек
}



#include "nvs_flash.h"  // добавьте в начало файла

void app_main(void) {
    // Инициализация NVS (ОБЯЗАТЕЛЬНО!)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    ESP_LOGI(TAG, "🚀 Запуск Смотрящего (монитор 15 ламп)");
    
    init_espnow();
    init_timer();
    
    // Вывод MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "📡 MAC адрес: %02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}