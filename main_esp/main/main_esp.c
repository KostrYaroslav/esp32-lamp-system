#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"

static const char *TAG = "MAIN_ESP";

// ==================== MAC-АДРЕСА ====================
static const uint8_t monitor_mac[6]    = {0x44, 0x1D, 0x64, 0xF6, 0x91, 0x34}; // Смотрящий
static const uint8_t controller_mac[6] = {0x68, 0xFE, 0x71, 0x88, 0x8E, 0x48}; // Управляющий

// ==================== НАСТРОЙКИ ====================
#define UART_GATEWAY_PORT   UART_NUM_1
#define UART_GATEWAY_TXD    4
#define UART_GATEWAY_RXD    5
#define BUF_SIZE            256
#define QUEUE_SIZE          10
#define WIFI_CHANNEL        1

// Код команды GET_STATE
#define CMD_GET_STATE       0x01

// ==================== СТРУКТУРЫ И ОЧЕРЕДИ ====================
typedef enum {
    TYPE_MASK_SET,
    TYPE_MASK_CLR,
    TYPE_GET_STATE
} command_type_t;

typedef struct {
    command_type_t type;
    uint32_t mask;
} command_msg_t;

static QueueHandle_t command_queue = NULL;
static SemaphoreHandle_t state_ready_semaphore = NULL; // Семафор для фиксации ответа от Смотрящего

// ==================== ОТПРАВКА СОСТОЯНИЯ ШЛЮЗУ ====================
static void send_state_to_gateway(uint32_t state) {
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "STATE %08" PRIX32 "\r\n", state);
    uart_write_bytes(UART_GATEWAY_PORT, buffer, strlen(buffer));
    ESP_LOGI(TAG, "📤 Состояние отправлено шлюзу: %08" PRIX32, state);
}

static void send_no_state_to_gateway(void) {
    uart_write_bytes(UART_GATEWAY_PORT, "NO_STATE\r\n", 10);
    ESP_LOGW(TAG, "⚠️ Отправлено шлюзу: NO_STATE");
}

static void send_ok_to_gateway(void) {
    uart_write_bytes(UART_GATEWAY_PORT, "OK\r\n", 4);
}

static void send_error_to_gateway(void) {
    uart_write_bytes(UART_GATEWAY_PORT, "ERROR\r\n", 7);
}

// ==================== ОТПРАВКА УПРАВЛЯЮЩЕМУ ====================
static void send_to_controller(uint32_t mask) {
    esp_err_t ret = esp_now_send(controller_mac, (const uint8_t*)&mask, sizeof(mask));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "📤 Маска отправлена Управляющему: %08" PRIX32, mask);
    } else {
        ESP_LOGE(TAG, "❌ Ошибка отправки Управляющему: %s", esp_err_to_name(ret));
    }
}

// ==================== ОТПРАВКА ЗАПРОСА СМОТРЯЩЕМУ ====================
static void send_get_state_to_monitor(void) {
    uint8_t cmd = CMD_GET_STATE;
    esp_err_t ret = esp_now_send(monitor_mac, &cmd, sizeof(cmd));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "🔍 Запрос GET_STATE отправлен Смотрящему");
    } else {
        ESP_LOGE(TAG, "❌ Ошибка отправки GET_STATE: %s", esp_err_to_name(ret));
    }
}

// ==================== ESP-NOW ПРИЁМ (от Смотрящего) ====================
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (info == NULL || data == NULL || len <= 0) return;
    if (memcmp(info->src_addr, monitor_mac, 6) != 0) return;

    if (len == 4) {
        uint32_t state;
        memcpy(&state, data, 4);
        ESP_LOGI(TAG, "📥 Получен статус от Смотрящего: %08" PRIX32, state);
        
        // Передаём состояние шлюзу
        send_state_to_gateway(state);

        // Разблокируем command_task, оповещая об успешном ответе
        if (state_ready_semaphore != NULL) {
            xSemaphoreGive(state_ready_semaphore);
        }
    }
}

// ==================== ESP-NOW ОТПРАВКА (колбэк) ====================
static void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGD(TAG, "✅ ESP-NOW отправлено успешно");
    } else {
        ESP_LOGW(TAG, "⚠️ ESP-NOW отправка не удалась");
    }
}

// ==================== ИНИЦИАЛИЗАЦИЯ ESP-NOW ====================
static void init_espnow(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));

    esp_now_peer_info_t peer = {0};
    peer.channel = WIFI_CHANNEL;
    peer.ifidx = 0;
    peer.encrypt = false;
    
    memcpy(peer.peer_addr, monitor_mac, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    memcpy(peer.peer_addr, controller_mac, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    
    ESP_LOGI(TAG, "✅ ESP-NOW инициализирован (мост)");
}

// ==================== ИНИЦИАЛИЗАЦИЯ UART ====================
static void init_uart(void) {
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    ESP_ERROR_CHECK(uart_param_config(UART_GATEWAY_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_GATEWAY_PORT, UART_GATEWAY_TXD, UART_GATEWAY_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_GATEWAY_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));
    
    ESP_LOGI(TAG, "✅ UART инициализирован (RX=%d, TX=%d)", UART_GATEWAY_RXD, UART_GATEWAY_TXD);
}

// ==================== UART ПРИЁМ (от Zigbee шлюза) ====================
static void uart_task(void *pvParameters) {
    uint8_t data[BUF_SIZE];
    char line[BUF_SIZE];
    int pos = 0;

    while (1) {
        int len = uart_read_bytes(UART_GATEWAY_PORT, data, sizeof(data) - 1, pdMS_TO_TICKS(50));
        for (int i = 0; i < len; i++) {
            if (data[i] == '\n' || data[i] == '\r') {
                if (pos > 0) {
                    line[pos] = '\0';
                    
                    // Парсинг команд
                    uint32_t mask = 0;
                    if (strncmp(line, "MASK_SET ", 9) == 0 && sscanf(line + 9, "%" SCNx32, &mask) == 1) {
                        command_msg_t msg = { .type = TYPE_MASK_SET, .mask = mask };
                        xQueueSend(command_queue, &msg, pdMS_TO_TICKS(10));
                    }
                    else if (strncmp(line, "MASK_CLR ", 9) == 0 && sscanf(line + 9, "%" SCNx32, &mask) == 1) {
                        command_msg_t msg = { .type = TYPE_MASK_CLR, .mask = mask };
                        xQueueSend(command_queue, &msg, pdMS_TO_TICKS(10));
                    }
                    else if (strcmp(line, "GET_STATE") == 0) {
                        command_msg_t msg = { .type = TYPE_GET_STATE, .mask = 0 };
                        xQueueSend(command_queue, &msg, pdMS_TO_TICKS(10));
                    }
                    else {
                        send_error_to_gateway();
                    }
                    pos = 0;
                }
            } else {
                if (pos < sizeof(line) - 1) {
                    line[pos++] = data[i];
                } else {
                    pos = 0;
                }
            }
        }
    }
}

// ==================== ОСНОВНАЯ ЗАДАЧА (обработка команд) ====================
static void command_task(void *pvParameters) {
    command_msg_t msg;
    
    while (1) {
        if (xQueueReceive(command_queue, &msg, portMAX_DELAY) == pdTRUE) {
            switch (msg.type) {
                case TYPE_MASK_SET:
                    ESP_LOGI(TAG, "⚙️ Команда MASK_SET: %08" PRIX32, msg.mask);
                    send_to_controller(msg.mask);
                    send_ok_to_gateway();
                    break;
                    
                case TYPE_MASK_CLR:
                    ESP_LOGI(TAG, "⚙️ Команда MASK_CLR: %08" PRIX32, msg.mask);
                    send_to_controller(msg.mask);
                    send_ok_to_gateway();
                    break;
                    
                case TYPE_GET_STATE:
                    ESP_LOGI(TAG, "🔍 Команда GET_STATE, опрашиваю Смотрящего");
                    
                    // Быстро сбрасываем семафор в 0, если там лежал старый фантомный флаг
                    xSemaphoreTake(state_ready_semaphore, 0);
                    
                    // Запрашиваем актуальный статус
                    send_get_state_to_monitor();
                    
                    // Блокируем таску максимум на 500 мс в ожидании ответа
                    if (xSemaphoreTake(state_ready_semaphore, pdMS_TO_TICKS(500)) == pdTRUE) {
                        // Успех: пакет получен, UART-ответ STATE уже отправлен внутри on_data_recv
                        ESP_LOGI(TAG, "✅ Ответ от Смотрящего получен вовремя");
                    } else {
                        // Таймаут: за 500 мс Смотрящий не прислал данные
                        ESP_LOGW(TAG, "⏱️ Таймаут ожидания Смотрящего. Ответ не получен");
                        send_no_state_to_gateway();
                    }
                    break;
            }
        }
    }
}

// ==================== MAIN ====================
void app_main(void) {
    // Инициализация NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Создание очереди команд
    command_queue = xQueueCreate(QUEUE_SIZE, sizeof(command_msg_t));
    if (command_queue == NULL) {
        ESP_LOGE(TAG, "❌ Ошибка создания очереди команд");
        return;
    }

    // Создание бинарного семафора для таймаутов
    state_ready_semaphore = xSemaphoreCreateBinary();
    if (state_ready_semaphore == NULL) {
        ESP_LOGE(TAG, "❌ Ошибка создания семафора");
        return;
    }

    // Инициализация интерфейсов
    init_uart();
    init_espnow();

    // Вывод лога успешного старта
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "🟢 Главный (ESP32-C6) запущен в режиме МОСТА");
    ESP_LOGI(TAG, "📡 MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "⚙️ Функции: ретрансляция команд, опрос Смотрящего с таймаутом");
    ESP_LOGI(TAG, "═══════════════════════════════════════════");

    // Запуск задач FreeRTOS
    xTaskCreate(uart_task, "uart_task", 4096, NULL, 5, NULL);
    xTaskCreate(command_task, "command_task", 4096, NULL, 5, NULL);
}
