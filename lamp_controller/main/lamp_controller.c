#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "driver/gpio.h"

static const char *TAG = "CONTROLLER";

// ==================== SN74HC595 ПИНЫ ====================
#define HC595_DATA_PIN   14
#define HC595_CLOCK_PIN  13
#define HC595_LATCH_PIN  12
#define HC595_OE_PIN     5

// ==================== ESP-NOW ПАРАМЕТРЫ ====================
static uint8_t main_mac[6] = {0x20, 0x6E, 0xF1, 0x13, 0x99, 0xE4};
#define WIFI_CHANNEL      1
#define QUEUE_SIZE        10

// ==================== ПАРАМЕТРЫ ИМПУЛЬСА ====================
#define PULSE_DURATION_MS 500

// Структура пакета от Главного (ровно 5 байт)
typedef struct {
    uint32_t mask;
    uint8_t action;  // 1 = Начать импульс (ON), 0 = Экстренное выключение (OFF)
} __attribute__((packed)) controller_pkt_t;

// ==================== СИСТЕМНЫЕ ОБЪЕКТЫ ====================
static QueueHandle_t mask_queue = NULL;
static uint32_t shift_reg_state = 0;

// ==================== SN74HC595 ФУНКЦИИ ====================

static void hc595_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << HC595_DATA_PIN)  | 
                        (1ULL << HC595_CLOCK_PIN) |
                        (1ULL << HC595_LATCH_PIN) |
                        (1ULL << HC595_OE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    gpio_set_level(HC595_CLOCK_PIN, 0);
    gpio_set_level(HC595_LATCH_PIN, 0);
    gpio_set_level(HC595_OE_PIN, 0); // Разрешаем работу выходов (низкий уровень)
    
    ESP_LOGI(TAG, "✅ SN74HC595 инициализирован (32 канала)");
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
    // Отправляем 4 байта (32 бита) последовательно, начиная со старшего байта
    for (int i = 3; i >= 0; i--) {
        hc595_write_byte((shift_reg_state >> (i * 8)) & 0xFF);
    }
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

// ==================== ESP-NOW ПРИЁМ ====================

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len == sizeof(controller_pkt_t)) {
        controller_pkt_t pkt;
        memcpy(&pkt, data, sizeof(pkt));
        
        ESP_LOGI(TAG, "📨 ESP-NOW пакет: Маска = %08" PRIX32 ", Действие = %d (%s)", 
                 pkt.mask, pkt.action, pkt.action ? "ЗАПУСК" : "СБРОС");
        
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        
        // Отправляем всю структуру пакета в очередь обработки
        if (xQueueSendFromISR(mask_queue, &pkt, &xHigherPriorityTaskWoken) != pdTRUE) {
            ESP_LOGW(TAG, "⚠️ Очередь команд переполнена! Пакет пропущен.");
        }
        
        // Мгновенное переключение на задачу command_task, если у неё выше приоритет
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    } else {
        ESP_LOGW(TAG, "❌ Ошибка: Получен пакет неверной длины: %d (ожидалось %d)", 
                 len, (int)sizeof(controller_pkt_t));
    }
}

static void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    ESP_LOGD(TAG, "ESP-NOW статус отправки: %s", status == ESP_NOW_SEND_SUCCESS ? "Успех" : "Ошибка");
}

static void init_espnow(void) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));
    
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    
    esp_now_peer_info_t peer = {
        .channel = WIFI_CHANNEL,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, main_mac, 6);
    
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_LOGI(TAG, "✅ Передатчик (Главный) успешно добавлен в Peer");
}

// ==================== ПОТОК ОБРАБОТКИ КОМАНД ====================

static void command_task(void *pvParameters) {
    uint32_t active_mask = 0;
    TickType_t pulse_duration_ticks = pdMS_TO_TICKS(PULSE_DURATION_MS);
    controller_pkt_t cmd;

    while (1) {
        // Если импульс неактивен — спим вечно (portMAX_DELAY).
        // Если импульс идет — ждем новую команду не дольше, чем длительность импульса.
        TickType_t wait_time = (active_mask == 0) ? portMAX_DELAY : pulse_duration_ticks;

        if (xQueueReceive(mask_queue, &cmd, wait_time) == pdTRUE) {
            if (cmd.action == 1) {
                // ЗАПУСК ИМПУЛЬСА: Объединяем новую маску с уже активными реле
                active_mask |= cmd.mask;
                hc595_set_channels(active_mask, true);
                ESP_LOGI(TAG, "🔛 Импульс активирован для маски: %08" PRIX32 " (Всего активно: %d реле)", 
                         cmd.mask, __builtin_popcount(active_mask));
            } else if (cmd.action == 0) {
                // ЭКСТРЕННЫЙ СБРОС: Выключаем конкретные реле по маске до истечения таймера
                hc595_set_channels(cmd.mask, false);
                active_mask &= ~cmd.mask;
                ESP_LOGI(TAG, "🛑 Экстренная отмена для маски: %08" PRIX32, cmd.mask);
            }
        } else {
            // Тайм-аут ожидания: Сработал программный замер длительности импульса
            if (active_mask != 0) {
                hc595_set_channels(active_mask, false);
                ESP_LOGI(TAG, "🔴 Время импульса истекло. Все активные реле выключены.");
                active_mask = 0;
            }
        }
    }
}

// ==================== MAIN ====================

void app_main(void) {
    // Инициализация NVS памяти (необходима для работы Wi-Fi стека)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Создаем очередь для передачи полных структур пакетов
    mask_queue = xQueueCreate(QUEUE_SIZE, sizeof(controller_pkt_t));
    if (mask_queue == NULL) {
        ESP_LOGE(TAG, "❌ Критическая ошибка: Не удалось создать очередь FreeRTOS");
        return;
    }

    // Настройка периферии
    hc595_init();
    hc595_set_channels(0xFFFFFFFF, false); // Гарантированно гасим все реле при старте контроллера
    ESP_LOGI(TAG, "🔄 Все каналы принудительно сброшены в 0");

    // Запуск беспроводной связи
    init_espnow();

    // Вывод лога успешного запуска с текущим MAC-адресом
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "🟢 Контроллер реле успешно запущен!");
    ESP_LOGI(TAG, "📡 Собственный MAC-адрес: %02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "⏱️ Заданная длительность импульса: %d мс", PULSE_DURATION_MS);
    ESP_LOGI(TAG, "═══════════════════════════════════════════");

    // Создаем высокоприоритетную задачу для управления реле (приоритет 10)
    xTaskCreate(command_task, "command_task", 4096, NULL, 10, NULL);

    // Главный поток app_main больше не нужен — отправляем его в бесконечный сон
    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
}
