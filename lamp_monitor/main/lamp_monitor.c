#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"         // Добавили семафоры для защиты шины
#include "driver/i2c_master.h"       
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"

static const char *TAG = "MONITOR";

// ==================== НАСТРОЙКИ ПЛАТЫ ====================
#define RELAY_COUNT         32
#define WIFI_CHANNEL        1

#define I2C_MASTER_SDA_IO   21
#define I2C_MASTER_SCL_IO   22
#define I2C_MASTER_FREQ_HZ  100000

#define TCA9555_ADDR_CHIP1  0x20 
#define TCA9555_ADDR_CHIP2  0x21 

#define TCA9555_REG_INPUT_0  0x00 
#define TCA9555_REG_CONFIG_0 0x06 

static const uint8_t main_mac[] = {0x20, 0x6E, 0xF1, 0x13, 0x99, 0xE4};

typedef enum {
    EV_SEND_REQUESTED, 
    EV_STATE_CHANGED   
} event_type_t;

static QueueHandle_t event_queue = NULL;
static SemaphoreHandle_t i2c_mutex = NULL; // Мьютекс для защиты конкурентного доступа

static _Atomic uint32_t lamp_state = ATOMIC_VAR_INIT(0x00000000);

static i2c_master_bus_handle_t  bus_handle  = NULL;
static i2c_master_dev_handle_t dev_handle1 = NULL;
static i2c_master_dev_handle_t dev_handle2 = NULL;

// НАСТРОЙКА ЧИПОВ
static esp_err_t init_pw555_inputs(void) {
    // Включаем авто-восстановление шины при залипании SDA
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_1,                 
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,                
        .flags.enable_internal_pullup = false, 
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Ошибка создания шины: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_config1 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9555_ADDR_CHIP1,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(bus_handle, &dev_config1, &dev_handle1);
    if (err != ESP_OK) return err;

    i2c_device_config_t dev_config2 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9555_ADDR_CHIP2,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(bus_handle, &dev_config2, &dev_handle2);
    if (err != ESP_OK) return err;

    uint8_t cfg_data[] = {TCA9555_REG_CONFIG_0, 0xFF, 0xFF}; 
    
    // Передаем конфигурацию с огромным таймаутом (500 мс) для надежности при первом включении
    err = i2c_master_transmit(dev_handle1, cfg_data, sizeof(cfg_data), pdMS_TO_TICKS(500));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Чип 1 не ответил на конфигурацию: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_master_transmit(dev_handle2, cfg_data, sizeof(cfg_data), pdMS_TO_TICKS(500));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Чип 2 не ответил на конфигурацию: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "✅ Безопасный режим НАБЛЮДЕНИЯ для PW555 успешно запущен");
    return ESP_OK;
}

// Потокобезопасный метод чтения с увеличенными таймаутами
static uint32_t read_hardware_state(void) {
    uint16_t chip1_state = 0xFFFF;
    uint16_t chip2_state = 0xFFFF;
    uint8_t reg_addr = TCA9555_REG_INPUT_0;
    uint8_t data[2] = {0};

    // Защищаем транзакцию мьютексом
    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        
        // Чтение Чипа №1 (Таймаут поднят до 200 мс)
        esp_err_t err = i2c_master_transmit_receive(dev_handle1, &reg_addr, 1, data, 2, pdMS_TO_TICKS(200));
        if (err == ESP_OK) {
            chip1_state = data[0] | (data[1] << 8);
        } else {
            ESP_LOGE(TAG, "❌ Таймаут чтения чипа 1 (0x%02X)", TCA9555_ADDR_CHIP1);
        }

        // Чтение Чипа №2
        err = i2c_master_transmit_receive(dev_handle2, &reg_addr, 1, data, 2, pdMS_TO_TICKS(200));
        if (err == ESP_OK) {
            chip2_state = data[0] | (data[1] << 8);
        } else {
            ESP_LOGE(TAG, "❌ Таймаут чтения чипа 2 (0x%02X)", TCA9555_ADDR_CHIP2);
        }

        xSemaphoreGive(i2c_mutex);
    }

    return ((uint32_t)chip2_state << 16) | chip1_state;
}

// ==================== ОСТАЛЬНАЯ СИСТЕМНАЯ ЛОГИКА ====================

static void monitor_poll_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60)); // Немного увеличили шаг, чтобы шина "отдыхала"
        
        uint32_t new_state = read_hardware_state();
        uint32_t old_state = atomic_load(&lamp_state);

        if (new_state != old_state) {
            atomic_store(&lamp_state, new_state);
            event_type_t ev = EV_STATE_CHANGED;
            xQueueSend(event_queue, &ev, pdMS_TO_TICKS(10));
        }
    }
}

static void send_status(uint32_t state) {
    esp_err_t ret = esp_now_send(main_mac, (const uint8_t*)&state, sizeof(state));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "📤 Маска отправлена Главному: 0x%08" PRIX32, state);
    } else {
        ESP_LOGE(TAG, "❌ Ошибка отправки ESP-NOW: %s", esp_err_to_name(ret));
    }
}

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (info == NULL || data == NULL || len <= 0) return;
    if (memcmp(info->src_addr, main_mac, 6) != 0) return;

    if (len == 1 && data[0] == 0x01) {
        event_type_t ev = EV_SEND_REQUESTED;
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendFromISR(event_queue, &ev, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

static void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "⚠️ Главный не подтвердил прием пакета");
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
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, main_mac, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_LOGI(TAG, "📡 ESP-NOW сеть успешно запущена");
}

static void monitor_tx_task(void *pvParameters) {
    event_type_t ev;
    while (1) {
        if (xQueueReceive(event_queue, &ev, portMAX_DELAY) == pdTRUE) {
            uint32_t current_state = atomic_load(&lamp_state);

            if (ev == EV_SEND_REQUESTED) {
                ESP_LOGI(TAG, "🔍 Запрос от Главного. Отправка среза: 0x%08" PRIX32, current_state);
                send_status(current_state);
            }
            else if (ev == EV_STATE_CHANGED) {
                ESP_LOGW(TAG, "📢 Фиксация изменения ламп! Новая маска: 0x%08" PRIX32, current_state);
                send_status(current_state);
            }
        }
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    event_queue = xQueueCreate(10, sizeof(event_type_t));
    if (event_queue == NULL) return;

    // Создаем мьютекс для I2C до старта задач
    i2c_mutex = xSemaphoreCreateMutex();
    if (i2c_mutex == NULL) return;

    // 1. Инициализация
    init_pw555_inputs();

    // 2. Первичное чтение
    uint32_t initial_state = read_hardware_state();
    atomic_store(&lamp_state, initial_state);
    ESP_LOGI(TAG, "📊 Начальный срез схемы мониторинга: 0x%08" PRIX32, initial_state);

    // 3. Запуск потоков
    xTaskCreate(monitor_tx_task, "monitor_tx_task", 4096, NULL, 5, NULL);
    xTaskCreate(monitor_poll_task, "monitor_poll_task", 4096, NULL, 4, NULL);

    // 4. Старт радио
    init_espnow();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "🟢 Смотрящий запущен в РЕЖИМЕ ЧИСТОГО НАБЛЮДЕНИЯ (Защищенный I2C)");
    ESP_LOGI(TAG, "📡 Мой MAC: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
}
