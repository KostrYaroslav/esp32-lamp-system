#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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
#define RELAY_COUNT        32
#define WIFI_CHANNEL       1

// ==================== ESP-NOW ====================
// MAC Главного (ESP32-C6 №2)
static const uint8_t main_mac[6] = {0x20, 0x6E, 0xF1, 0x13, 0x99, 0xE4};

// ==================== ОЧЕРЕДЬ СОБЫТИЙ ====================
typedef enum {
    EV_SEND_REQUESTED
} event_type_t;

static QueueHandle_t event_queue = NULL;

// ==================== СОСТОЯНИЕ ЛАМП ====================
// ФИКСИРОВАННОЕ СОСТОЯНИЕ: лампы 1, 3, 17 включены
// Бит 0 = лампа 1 (1 << 0) = 0x00000001
// Бит 2 = лампа 3 (1 << 2) = 0x00000004
// Бит 16 = лампа 17 (1 << 16) = 0x00010000
// Итого: 0x00010005
static _Atomic uint32_t lamp_state = ATOMIC_VAR_INIT(0x00010005);

// ==================== ОТПРАВКА СТАТУСА ====================
static void send_status(uint32_t state) {
    esp_err_t ret = esp_now_send(main_mac, (const uint8_t*)&state, sizeof(state));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "📤 Статус отправлен: %08" PRIX32, state);
        ESP_LOGI(TAG, "   Включены лампы: 1, 3, 17");
    } else {
        ESP_LOGE(TAG, "❌ Ошибка отправки: %s", esp_err_to_name(ret));
    }
}

// ==================== ОБРАБОТКА ЗАПРОСА ОТ ГЛАВНОГО ====================
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (info == NULL || data == NULL || len <= 0) return;
    if (memcmp(info->src_addr, main_mac, 6) != 0) return;

    if (len == 1 && data[0] == 0x01) {
        event_type_t ev = EV_SEND_REQUESTED;
        xQueueSendFromISR(event_queue, &ev, NULL);
    }
}

// ==================== ESP-NOW ====================
static void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGD(TAG, "✅ Доставлено");
    } else {
        ESP_LOGW(TAG, "⚠️ Не доставлено");
    }
}

static void init_espnow(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    esp_now_peer_info_t peer = {
        .channel = WIFI_CHANNEL,
        .ifidx = 0,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, main_mac, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    
    ESP_LOGI(TAG, "📡 Peer Главного добавлен");
}

// ==================== ЗАДАЧА ОТПРАВКИ ====================
static void monitor_tx_task(void *pvParameters) {
    event_type_t ev;
    while (1) {
        if (xQueueReceive(event_queue, &ev, portMAX_DELAY) == pdTRUE) {
            uint32_t current_state = atomic_load(&lamp_state);
            ESP_LOGI(TAG, "🔍 Отправка по запросу: %08" PRIX32, current_state);
            send_status(current_state);
        }
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

    event_queue = xQueueCreate(10, sizeof(event_type_t));
    if (event_queue == NULL) {
        ESP_LOGE(TAG, "❌ Ошибка создания очереди");
        return;
    }

    xTaskCreate(monitor_tx_task, "monitor_tx_task", 4096, NULL, 5, NULL);

    init_espnow();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "🟢 Смотрящий запущен (ФИКСИРОВАННОЕ СОСТОЯНИЕ)");
    ESP_LOGI(TAG, "📡 MAC: %02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "💡 Постоянное состояние: лампы 1, 3, 17 включены");
    ESP_LOGI(TAG, "🔍 Отвечаю только на запросы GET_STATE");
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    
    // Таймер ОТКЛЮЧЁН — состояние никогда не меняется
}