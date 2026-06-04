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
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_event.h"

static const char *TAG = "CONTROLLER";

// ==================== SN74HC595 ПИНЫ ====================
#define HC595_DATA_PIN  14
#define HC595_CLOCK_PIN 13
#define HC595_LATCH_PIN 12
#define HC595_OE_PIN    5

// ==================== ESP-NOW ПАРАМЕТРЫ ====================
static const uint8_t main_mac[6] = {0x20, 0x6E, 0xF1, 0x13, 0x99, 0xE4};
#define WIFI_CHANNEL 1
#define QUEUE_SIZE 20
#define PULSE_DURATION_MS 500

// ==================== ПРОТОКОЛ ДИАЛОГА ====================
// Команды от Главного к Управляющему
typedef struct {
    uint32_t mask;
    uint8_t action; // 0x01 = EXEC, 0xFF = CONFIRM
} __attribute__((packed)) controller_pkt_t;

// Ответы от Управляющего к Главному
typedef struct {
    uint8_t step;  // 0x02 = ACK (Да), 0x03 = ECHO (Лампу X), 0x04 = DONE (Включил)
    uint32_t mask;
} __attribute__((packed)) response_pkt_t;

// ==================== СИСТЕМНЫЕ ОБЪЕКТЫ ====================
static QueueHandle_t mask_queue = NULL;
static uint32_t shift_reg_state = 0;
static portMUX_TYPE reg_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t channel_timers[32];
static uint32_t pending_mask = 0;

// Прототипы функций
static void channel_timeout_callback(void* arg);
static void send_response(uint8_t step, uint32_t mask);
static void hc595_set_channels(uint32_t mask, bool state);

// ==================== SN74HC595 ФУНКЦИИ ====================
static void hc595_init(void) {
    // Блокируем выходы до очистки регистра
    gpio_set_level(HC595_OE_PIN, 1);
    
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
    
    gpio_set_level(HC595_CLOCK_PIN, 0);
    gpio_set_level(HC595_LATCH_PIN, 0);

    // Очищаем регистры (записываем 32 нуля)
    for (int i = 0; i < 32; i++) {
        gpio_set_level(HC595_DATA_PIN, 0);
        gpio_set_level(HC595_CLOCK_PIN, 1);
        gpio_set_level(HC595_CLOCK_PIN, 0);
    }
    gpio_set_level(HC595_LATCH_PIN, 1);
    
    // Разрешаем выходы
    gpio_set_level(HC595_OE_PIN, 0);
    ESP_LOGI(TAG, "✅ SN74HC595 инициализирован (32 канала)");
}

static inline void hc595_write_byte(uint8_t data) {
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(HC595_DATA_PIN, (data >> i) & 1);
        gpio_set_level(HC595_CLOCK_PIN, 1);
        gpio_set_level(HC595_CLOCK_PIN, 0);
    }
}

static void hc595_update(uint32_t state) {
    gpio_set_level(HC595_LATCH_PIN, 0);
    hc595_write_byte((state >> 24) & 0xFF);
    hc595_write_byte((state >> 16) & 0xFF);
    hc595_write_byte((state >> 8) & 0xFF);
    hc595_write_byte(state & 0xFF);
    gpio_set_level(HC595_LATCH_PIN, 1);
}

static void hc595_set_channels(uint32_t mask, bool state) {
    uint32_t local_state;
    portENTER_CRITICAL(&reg_mux);
    if (state) {
        shift_reg_state |= mask;
    } else {
        shift_reg_state &= ~mask;
    }
    local_state = shift_reg_state;
    portEXIT_CRITICAL(&reg_mux);
    hc595_update(local_state);
}

// Коллбэк таймера: выключает конкретное реле
static void channel_timeout_callback(void* arg) {
    uint32_t channel_idx = (uint32_t)(uintptr_t)arg;
    uint32_t mask = (1UL << channel_idx);
    hc595_set_channels(mask, false);
}

static void init_channel_timers(void) {
    for (uint32_t i = 0; i < 32; i++) {
        esp_timer_create_args_t timer_args = {
            .callback = &channel_timeout_callback,
            .arg = (void*)(uintptr_t)i,
            .name = "ch_timer"
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &channel_timers[i]));
    }
    ESP_LOGI(TAG, "✅ Инициализировано 32 таймера");
}

// ==================== ОТПРАВКА ОТВЕТА ====================
static void send_response(uint8_t step, uint32_t mask) {
    response_pkt_t resp = { .step = step, .mask = mask };
    // Приведение типов (uint8_t*) убирает предупреждение о потере const
    esp_err_t err = esp_now_send((uint8_t*)main_mac, (uint8_t*)&resp, sizeof(resp));
    if (err == ESP_OK) {
        const char* step_str = "";
        if (step == 0x02) step_str = "Да";
        else if (step == 0x03) step_str = "Лампу";
        else if (step == 0x04) step_str = "Включил";
        ESP_LOGI(TAG, "📤 Ответ: %s (маска %08" PRIX32 ")", step_str, mask);
    } else {
        ESP_LOGE(TAG, "❌ Ошибка отправки ответа: %s", esp_err_to_name(err));
    }
}

// ==================== ESP-NOW ПРИЁМ ====================
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (memcmp(info->src_addr, main_mac, 6) != 0) return;

    // Шаг 1: PING от Главного обрабатываем сразу (исправлен синтаксис data[0])
    if (len == 1 && data[0] == 0x01) {
        send_response(0x02, 0); // Ответ "Да"
        return;
    }

    if (len == sizeof(controller_pkt_t)) {
        controller_pkt_t pkt;
        memcpy(&pkt, data, sizeof(pkt));
        
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendFromISR(mask_queue, &pkt, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

// ==================== ESP-NOW ИНИЦИАЛИЗАЦИЯ ====================
static void init_espnow(void) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Создаём дефолтный интерфейс (исправляет пустую работу ESP-NOW)
    esp_netif_create_default_wifi_sta();
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));
    
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));
    
    esp_now_peer_info_t peer = {
        .channel = WIFI_CHANNEL,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, main_mac, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_LOGI(TAG, "✅ Peer Главного добавлен");
}

// ==================== ЗАДАЧА ОБРАБОТКИ КОМАНД ====================
static void command_task(void *pvParameters) {
    controller_pkt_t cmd;
    while (1) {
        // Ожидаем данные из очереди (блокирующий режим)
        if (xQueueReceive(mask_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            
            // Шаг 3: CONFIRM от Главного
            if (cmd.action == 0xFF) {
                if (cmd.mask == pending_mask && pending_mask != 0) {
                    // Выполняем команду: включаем указанные каналы
                    hc595_set_channels(cmd.mask, true);
                    
                    for (int i = 0; i < 32; i++) {
                        if ((cmd.mask >> i) & 1) {
                            esp_timer_stop(channel_timers[i]);
                            esp_timer_start_once(channel_timers[i], PULSE_DURATION_MS * 1000ULL);
                        }
                    }
                    // Ответ "Включил"
                    send_response(0x04, cmd.mask);
                    ESP_LOGI(TAG, "⚡ Импульс по маске: %08" PRIX32, cmd.mask);
                    pending_mask = 0;
                }
            }
            // Шаг 2: EXEC от Главного (первая команда)
            else {
                pending_mask = cmd.mask;
                // Ответ "Лампу X" (ECHO)
                send_response(0x03, cmd.mask);
            }
        }
    }
}

// ==================== MAIN ====================
void app_main(void) {
    // 1. Сразу аппаратно блокируем выходы реле через OE пин
    gpio_config_t oe_conf = {
        .pin_bit_mask = (1ULL << HC595_OE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&oe_conf);
    gpio_set_level(HC595_OE_PIN, 1);

    // 2. Инициализация энергонезависимой памяти (NVS)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 3. Инициализация глобальных сетевых интерфейсов (Вызывается строго ОДИН раз здесь)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 4. Создание межзадачной очереди
    mask_queue = xQueueCreate(QUEUE_SIZE, sizeof(controller_pkt_t));
    if (mask_queue == NULL) {
        ESP_LOGE(TAG, "❌ Ошибка создания очереди");
        return;
    }

    // 5. Настройка сдвиговых регистров SN74HC595 и таймеров каналов
    hc595_init();
    init_channel_timers();
    hc595_set_channels(0xFFFFFFFF, false);
    ESP_LOGI(TAG, "🔄 Все реле принудительно выключены");

    // 6. Запуск сетевого протокола ESP-NOW
    init_espnow();

    // 7. Чтение и логирование физического MAC-адреса устройства
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "🟢 Управляющий запущен (ИСПОЛНИТЕЛЬ + ДИАЛОГ)");
    ESP_LOGI(TAG, "📡 MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "⚡ Длительность импульса: %d мс", PULSE_DURATION_MS);
    ESP_LOGI(TAG, "═══════════════════════════════════════════");

    // 8. Запуск основной обрабатывающей задачи на Core 1
    xTaskCreatePinnedToCore(command_task, "command_task", 8192, NULL, 10, NULL, 1);
    
    // 9. Разрешаем выходы чипа, когда вся периферия полностью готова к работе
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(HC595_OE_PIN, 0);
    ESP_LOGI(TAG, "✅ Управляющий полностью готов к работе");
    }