#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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

// Структура для передачи команд
typedef struct {
    uint32_t mask;
    uint8_t action; // 0 = SET, 1 = CLR
} command_msg_t;

static QueueHandle_t mask_queue = NULL;
static _Atomic uint32_t lamp_state = ATOMIC_VAR_INIT(0x00000000);
static volatile TaskHandle_t command_task_handle = NULL;

// ==================== ОТПРАВКА СОСТОЯНИЯ ШЛЮЗУ ====================
static void send_state_to_gateway(uint32_t state) {
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "STATE %08" PRIX32 "\r\n", state);
    uart_write_bytes(UART_GATEWAY_PORT, buffer, strlen(buffer));
    ESP_LOGI(TAG, "📤 Состояние отправлено шлюзу: %08" PRIX32, state);
}

// ==================== ESP-NOW CALLBACKS ====================
static void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (tx_info != NULL && memcmp(tx_info->des_addr, controller_mac, 6) == 0) {
        // Локальная копия защищает от Race Condition при смене контекста
        TaskHandle_t task_to_notify = command_task_handle;
        if (task_to_notify != NULL) {
            uint32_t notify_val = (status == ESP_NOW_SEND_SUCCESS) ? 1 : 2;
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            
            xTaskNotifyFromISR(task_to_notify, notify_val, eSetValueWithOverwrite, &xHigherPriorityTaskWoken);
            
            if (xHigherPriorityTaskWoken == pdTRUE) {
                portYIELD_FROM_ISR();
            }
        }
    }
}

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (info == NULL || data == NULL || len <= 0) return;
    if (memcmp(info->src_addr, monitor_mac, 6) != 0) return;

    if (len == 4) {
        uint32_t temp;
        memcpy(&temp, data, 4);
        atomic_store(&lamp_state, temp);
        ESP_LOGI(TAG, "📥 Статус от Смотрящего: %08" PRIX32, temp);
        send_state_to_gateway(temp);
    } else if (len == 2) {
        uint16_t old_state;
        memcpy(&old_state, data, 2);
        uint32_t new_state = (uint32_t)old_state;
        atomic_store(&lamp_state, new_state);
        ESP_LOGI(TAG, "📥 Статус от Смотрящего (16 бит): %04X", old_state);
        send_state_to_gateway(new_state);
    }
}

// ==================== ESP-NOW ОТПРАВКА ====================
static bool send_mask_blocking(uint32_t mask) {
    uint32_t status_code = 0;
    
    // Очищаем старые уведомления перед отправкой нового пакета
    xTaskNotifyWait(ULONG_MAX, ULONG_MAX, &status_code, 0);
    
    esp_err_t ret = esp_now_send(controller_mac, (const uint8_t*)&mask, sizeof(mask));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Ошибка esp_now_send: %s", esp_err_to_name(ret));
        return false;
    }

    // Блокирующий таймаут ожидания подтверждения доставки
    BaseType_t notified = xTaskNotifyWait(ULONG_MAX, ULONG_MAX, &status_code, pdMS_TO_TICKS(500));
    
    if (notified == pdFALSE) {
        ESP_LOGE(TAG, "❌ Таймаут ожидания ACK от Управляющего");
        return false;
    }

    if (status_code == 1) {
        ESP_LOGI(TAG, "📤 Маска %08" PRIX32 " доставлена", mask);
        return true;
    } else {
        ESP_LOGE(TAG, "❌ Нет ACK от Управляющего (ошибка доставки)");
        return false;
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
    
    ESP_LOGI(TAG, "✅ ESP-NOW инициализирован");
}

// ==================== UART ====================
static void send_uart_response(const char *msg) {
    uart_write_bytes(UART_GATEWAY_PORT, msg, strlen(msg));
    uart_write_bytes(UART_GATEWAY_PORT, "\r\n", 2);
}

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
                    uint32_t parsed_mask = 0;
                    int action = -1;
                    
                    if (strncmp(line, "MASK_SET ", 9) == 0 && sscanf(line + 9, "%" SCNx32, &parsed_mask) == 1) {
                        action = 0;
                    }
                    else if (strncmp(line, "MASK_CLR ", 9) == 0 && sscanf(line + 9, "%" SCNx32, &parsed_mask) == 1) {
                        action = 1;
                    }
                    else {
                        send_uart_response("ERROR INVALID_SYNTAX");
                        pos = 0;
                        continue;
                    }
                    
                    command_msg_t msg;
                    msg.mask = parsed_mask;
                    msg.action = (uint8_t)action;
                    
                    if (xQueueSend(mask_queue, &msg, pdMS_TO_TICKS(10)) != pdTRUE) {
                        send_uart_response("ERROR QUEUE_FULL");
                    } else {
                        send_uart_response("OK");
                    }
                    pos = 0;
                }
            } else {
                if (pos < (sizeof(line) - 1)) {
                    line[pos++] = data[i];
                } else {
                    send_uart_response("ERROR BUFFER_OVERFLOW");
                    pos = 0; // Сброс буфера для защиты от переполнения
                }
            }
        }
    }
}

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

// ==================== ЛОГИКА ОБРАБОТКИ ====================
static void command_task(void *pvParameters) {
    command_msg_t msg;

    while (1) {
        if (xQueueReceive(mask_queue, &msg, portMAX_DELAY) == pdTRUE) {
            uint32_t current_state = atomic_load(&lamp_state);
            uint32_t actual_mask = 0;
            
            if (msg.action == 0) {
                // SET - включить лампы, которые выключены (инвертируем маску состояния)
                actual_mask = msg.mask & (~current_state);
                ESP_LOGI(TAG, "⚙️ SET: запрошено %08" PRIX32, msg.mask);
            } else {
                // CLR - выключить лампы, которые сейчас включены
                actual_mask = msg.mask & current_state;
                ESP_LOGI(TAG, "⚙️ CLR: запрошено %08" PRIX32, msg.mask);
            }
            
            if (actual_mask != 0) {
                ESP_LOGI(TAG, "   Текущее состояние: %08" PRIX32, current_state);
                ESP_LOGI(TAG, "   Отправляем импульс на: %08" PRIX32 " (%d реле)", 
                         actual_mask, __builtin_popcount(actual_mask));
                
                if (send_mask_blocking(actual_mask)) {
                    ESP_LOGI(TAG, "   ✅ Импульс успешно передан");
                } else {
                    ESP_LOGE(TAG, "   ❌ Не удалось передать импульс");
                }
            } else {
                ESP_LOGI(TAG, "   💡 Изменений не требуется (лампы уже в нужном состоянии)");
            }
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

    mask_queue = xQueueCreate(QUEUE_SIZE, sizeof(command_msg_t));
    if (mask_queue == NULL) {
        ESP_LOGE(TAG, "❌ Ошибка создания очереди");
        return;
    }

    // КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ: Таск создается до старта ESP-NOW. Указатель command_task_handle гарантированно валиден.
    xTaskCreate(command_task, "command_task", 4096, NULL, 5, (TaskHandle_t *)&command_task_handle);

    init_uart();
    init_espnow();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "🟢 Главный (ESP32-C6) запущен");
    ESP_LOGI(TAG, "📡 MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "⚙️ Режим: SET/CLR раздельно, обратная связь включена");
    ESP_LOGI(TAG, "📡 UART: RX=%d, TX=%d", UART_GATEWAY_RXD, UART_GATEWAY_TXD);
    ESP_LOGI(TAG, "═══════════════════════════════════════════");

    xTaskCreate(uart_task, "uart_task", 4096, NULL, 5, NULL);
}
