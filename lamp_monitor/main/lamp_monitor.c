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
// MAC Главного
static const uint8_t main_mac[6] = {0x20, 0x6E, 0xF1, 0x13, 0x99, 0xE4};

// ==================== ОЧЕРЕДЬ СОБЫТИЙ ====================
typedef enum {
    EV_SEND_REQUESTED,  // Задача 1: Запрос от Главного
    EV_STATE_CHANGED    // Задача 2: Реальное изменение состояния ламп
} event_type_t;

static QueueHandle_t event_queue = NULL;

// ==================== СОСТОЯНИЕ ЛАМП ====================
// Стартовое состояние: лампы 1, 3, 17 включены (0x00010005)
static _Atomic uint32_t lamp_state = ATOMIC_VAR_INIT(0x00010005);

// ==================== ОТПРАВКА СТАТУСА ====================
static void send_status(uint32_t state) {
    esp_err_t ret = esp_now_send(main_mac, (const uint8_t*)&state, sizeof(state));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "📤 Пакет ушел Главному. Маска: %08" PRIX32, state);
    } else {
        ESP_LOGE(TAG, "❌ Ошибка отправки: %s", esp_err_to_name(ret));
    }
}

// ==================== ЗАДАЧА 1: ОБРАБОТКА ЗАПРОСА ОТ ГЛАВНОГО ====================
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (info == NULL || data == NULL || len <= 0) return;
    if (memcmp(info->src_addr, main_mac, 6) != 0) return;

    // Главный прислал команду опроса (0x01)
    if (len == 1 && data[0] == 0x01) {
        event_type_t ev = EV_SEND_REQUESTED;
        // Толкаем в очередь из контекста прерывания (ISR)
        xQueueSendFromISR(event_queue, &ev, NULL);
    }
}

// ==================== ЗАДАЧА 2: ИМИТАЦИЯ ИЗМЕНЕНИЯ СОСТОЯНИЯ (ТАЙМЕР) ====================
// В реальном железе здесь будет опрос GPIO/микросхем расширителей.
// Если новое считанное состояние не совпадает со старым — вызываем этот триггер.
static void timer_callback(void* arg) {
    uint32_t mask_lamp_1 = (1UL << 0);
    
    // Атомарно инвертируем Лампу 1
    uint32_t old_state = atomic_fetch_xor(&lamp_state, mask_lamp_1);
    uint32_t new_state = old_state ^ mask_lamp_1;

    ESP_LOGW(TAG, "⏰ [Событие] Лампа 1 изменила состояние: %s -> %s",
             (old_state & mask_lamp_1) ? "ON" : "OFF",
             (new_state & mask_lamp_1) ? "ON" : "OFF");

    // Состояние изменилось! Отправляем сигнал в очередь
    event_type_t ev = EV_STATE_CHANGED;
    xQueueSend(event_queue, &ev, portMAX_DELAY);
}

// ==================== ESP-NOW CALLBACK СТАТУСА ====================
static void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGD(TAG, "✅ Доставлено Главному");
    } else {
        ESP_LOGW(TAG, "⚠️ Сбой доставки пакета Главному");
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
    
    ESP_LOGI(TAG, "📡 Связь с Главным настроена");
}

// ==================== ЕДИНАЯ СЛУЖБА ОТПРАВКИ МАСКИ ====================
static void monitor_tx_task(void *pvParameters) {
    event_type_t ev;
    while (1) {
        // Задача спит, пока в очереди нет событий
        if (xQueueReceive(event_queue, &ev, portMAX_DELAY) == pdTRUE) {
            
            // Читаем текущую полную маску всех 32 ламп
            uint32_t current_state = atomic_load(&lamp_state);
            
            if (ev == EV_SEND_REQUESTED) {
                ESP_LOGI(TAG, "🔍 [Задача 1] Ответ по запросу Главного: %08" PRIX32, current_state);
            } 
            else if (ev == EV_STATE_CHANGED) {
                ESP_LOGW(TAG, "📢 [Задача 2] Инициативный доклад об изменении ламп: %08" PRIX32, current_state);
            }
            
            // Отправляем всю маску
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

    // Создаем очередь для событий
    event_queue = xQueueCreate(10, sizeof(event_type_t));
    if (event_queue == NULL) {
        ESP_LOGE(TAG, "❌ Ошибка создания очереди событий");
        return;
    }

    // Запускаем диспетчер отправки
    xTaskCreate(monitor_tx_task, "monitor_tx_task", 4096, NULL, 5, NULL);

    init_espnow();

    // Инициализация периодического таймера для имитации Задача 2 (раз в 30 сек)
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &timer_callback,
        .name = "periodic_lamp_timer"
    };
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 30ULL * 1000000ULL));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "🟢 Смотрящий запущен в ГИБРИДНОМ режиме");
    ESP_LOGI(TAG, "📡 Мой MAC: %02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "1️⃣ Функция 1: Стенд отвечает на запросы GET_STATE (0x01)");
    ESP_LOGI(TAG, "2️⃣ Функция 2: При изменении любой лампы шлет маску сам");
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
}
