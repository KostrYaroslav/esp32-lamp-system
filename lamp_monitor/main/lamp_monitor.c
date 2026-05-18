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
#define SEND_INTERVAL_MS   30000           
#define WIFI_CHANNEL       1

// ==================== ESP-NOW ====================
static const uint8_t main_mac[6] = {0x20, 0x6E, 0xF1, 0x13, 0x99, 0xE4};  

// ==================== ОЧЕРЕДЬ СОБЫТИЙ ====================
typedef enum {
    EV_SEND_SPONTANEOUS,
    EV_SEND_REQUESTED
} event_type_t;

static QueueHandle_t event_queue = NULL;

// ==================== СОСТОЯНИЕ ЛАМП ====================
static _Atomic uint32_t lamp_state = ATOMIC_VAR_INIT(0x55555555);  
static esp_timer_handle_t status_timer = NULL;
static uint32_t emulation_counter = 0x55555555;

static void generate_emulated_state(void) {
    emulation_counter = (emulation_counter << 1) | ((emulation_counter >> 31) & 1);
    atomic_store(&lamp_state, emulation_counter);
    ESP_LOGI(TAG, "🎲 Эмуляция: новое состояние %08" PRIX32, emulation_counter);
}

// ==================== ОТПРАВКА СТАТУСА ====================
static void send_status(uint32_t state) {
    esp_err_t ret = esp_now_send(main_mac, (const uint8_t*)&state, sizeof(state));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "📤 Статус отправлен: %08" PRIX32, state);
    } else {
        ESP_LOGE(TAG, "❌ Ошибка отправки: %s", esp_err_to_name(ret));
    }
}

// ==================== ЗАДАЧА ОБРАБОТКИ И ОТПРАВКИ ====================
static void monitor_tx_task(void *pvParameters) {
    event_type_t ev;
    while (1) {
        if (xQueueReceive(event_queue, &ev, portMAX_DELAY) == pdTRUE) {
            uint32_t current_state = atomic_load(&lamp_state);
            
            if (ev == EV_SEND_REQUESTED) {
                ESP_LOGI(TAG, "🔍 Обработка запроса, отправляю: %08" PRIX32, current_state);
            } else {
                ESP_LOGD(TAG, "⏰ Плановая отправка: %08" PRIX32, current_state);
            }
            
            send_status(current_state);
            
            // Если отправка была плановой по таймеру — обновляем состояние для следующего раза
            if (ev == EV_SEND_SPONTANEOUS) {
                generate_emulated_state();
            }
        }
    }
}

// ==================== ОБРАБОТКА ЗАПРОСА ОТ ГЛАВНОГО ====================
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (info == NULL || data == NULL || len <= 0) return;
    if (memcmp(info->src_addr, main_mac, 6) != 0) return;

    if (len == 1 && data[0] == 0x01) {  
        event_type_t ev = EV_SEND_REQUESTED;
        // Отправляем событие в очередь без блокировки
        xQueueSendFromISR(event_queue, &ev, NULL);
    }
}

// ==================== ESP-NOW CALLBACK СТАТУСА ====================
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
        .ifidx = WIFI_IF_STA, // Явно указываем интерфейс STA вместо 0
        .encrypt = false,
    };
    memcpy(peer.peer_addr, main_mac, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    
    ESP_LOGI(TAG, "📡 Peer Главного добавлен");
}

// ==================== ТАЙМЕР ДЛЯ ЭМУЛЯЦИИ ====================
static void timer_callback(void *arg) {
    event_type_t ev = EV_SEND_SPONTANEOUS;
    // Безопасно отправляем событие в очередь из прерывания таймера
    xQueueSendFromISR(event_queue, &ev, NULL);
}

static void init_timer(void) {
    esp_timer_create_args_t timer_args = {
        .callback = &timer_callback,
        .name = "status_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &status_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(status_timer, SEND_INTERVAL_MS * 1000));
}

// ==================== MAIN ====================
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Создаем очередь перед запуском задач и прерываний
    event_queue = xQueueCreate(10, sizeof(event_type_t));
    if (event_queue == NULL) {
        ESP_LOGE(TAG, "❌ Ошибка создания очереди");
        return;
    }

    // Запуск рабочей задачи для отправки сообщений (приоритет 5)
    xTaskCreate(monitor_tx_task, "monitor_tx_task", 4096, NULL, 5, NULL);

    init_espnow();
    
    atomic_store(&lamp_state, emulation_counter);
    
    init_timer();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "🟢 Смотрящий запущен (БЕЗОПАСНАЯ ОЧЕРЕДЬ)");
    ESP_LOGI(TAG, "📡 MAC: %02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "🎮 Отправка каждые %d секунд", SEND_INTERVAL_MS / 1000);
    ESP_LOGI(TAG, "🔍 Поддерживаются запросы GET_STATE");
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
}
