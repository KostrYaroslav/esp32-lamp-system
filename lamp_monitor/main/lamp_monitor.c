#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"

static const char *TAG = "MONITOR";

// ==================== НАСТРОЙКИ ====================
#define SEND_INTERVAL_MS   30000           // 30 секунд между отправками
#define WIFI_CHANNEL       1               // Фиксированный канал Wi-Fi

// ==================== ESP-NOW ====================
static const uint8_t main_mac[6] = {0x20, 0x6E, 0xF1, 0x13, 0x99, 0xE4};  // MAC Главного

// ==================== СОСТОЯНИЕ ЛАМП ====================
static _Atomic uint32_t lamp_state = ATOMIC_VAR_INIT(0x00000000);
static esp_timer_handle_t status_timer = NULL;

// ==================== ЭМУЛЯЦИЯ ====================
static uint32_t emulation_counter = 0x55555555;

static void generate_emulated_state(void) {
    // Корректный сброс счетчика при достижении максимального значения uint32_t
    if (emulation_counter == 0xFFFFFFFF) {
        emulation_counter = 0x55555555;
    } else {
        emulation_counter++;
    }
    
    atomic_store(&lamp_state, emulation_counter);
    ESP_LOGI(TAG, "🎲 Эмуляция: новое состояние %08" PRIX32, emulation_counter);
}

// ==================== ОТПРАВКА СТАТУСА ====================
static void send_status(void *arg) {
    uint32_t current_state = atomic_load(&lamp_state);
    esp_err_t ret = esp_now_send(main_mac, (const uint8_t*)&current_state, sizeof(current_state));
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "📤 Статус отправлен: %08" PRIX32, current_state);
    } else {
        ESP_LOGE(TAG, "❌ Ошибка отправки статуса: %s", esp_err_to_name(ret));
    }
    
    // Генерируем новое состояние для следующей отправки
    generate_emulated_state();
}

// ==================== ESP-NOW СЛУЖЕБНЫЕ ====================
// Правильная сигнатура для ESP-IDF v6.0.1
static void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGI(TAG, "✅ Данные доставлены на %02X:%02X:%02X:%02X:%02X:%02X",
                 tx_info->des_addr[0], tx_info->des_addr[1], tx_info->des_addr[2],
                 tx_info->des_addr[3], tx_info->des_addr[4], tx_info->des_addr[5]);
    } else {
        ESP_LOGW(TAG, "⚠️ Ошибка доставки (нет ACK)");
    }
}

static void init_espnow(void) {
    // Инициализация сетевого интерфейса и цикла событий (необходимы для Wi-Fi в v5.x)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Инициализация Wi-Fi в режиме Station
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));
    
    // Инициализация протокола ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    
    // Настройка пира (Главного устройства)
    esp_now_peer_info_t peer = {
        .channel = WIFI_CHANNEL,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, main_mac, 6);
    
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка добавления peer: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "📡 Peer Главного успешно добавлен");
    }
}

// ==================== ТАЙМЕР ====================
static void init_timer(void) {
    esp_timer_create_args_t timer_args = {
        .callback = &send_status,
        .name = "status_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &status_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(status_timer, (uint64_t)SEND_INTERVAL_MS * 1000));
}

// ==================== MAIN ====================
void app_main(void) {
    // Инициализация энергонезависимой памяти (NVS)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Запуск Wi-Fi и ESP-NOW
    init_espnow();
    
    // Генерация стартового состояния ламп
    generate_emulated_state();
    
    // Запуск таймера отправки
    init_timer();
    
    // Вывод собственного MAC-адреса для отладки
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "🟢 Монитор запущен (РЕЖИМ ЭМУЛЯЦИИ)");
    ESP_LOGI(TAG, "📡 Собственный MAC: %02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "🎮 Период отправки: %d секунд", SEND_INTERVAL_MS / 1000);
}
