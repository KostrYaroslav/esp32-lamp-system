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

static uint8_t monitor_mac[]    = {0x44, 0x1D, 0x64, 0xF6, 0x91, 0x34};
static uint8_t controller_mac[] = {0x68, 0xFE, 0x71, 0x88, 0x8E, 0x48};

#define UART_GATEWAY_PORT   UART_NUM_1
#define UART_GATEWAY_TXD    4
#define UART_GATEWAY_RXD    5
#define BUF_SIZE            256
#define QUEUE_SIZE          10
#define WIFI_CHANNEL        1 // Фиксированный канал для стабильности ESP-NOW

static QueueHandle_t mask_queue = NULL;
static _Atomic uint32_t lamp_state = ATOMIC_VAR_INIT(0x00000000); 

static void send_mask(uint32_t mask) {
    esp_err_t ret = esp_now_send(controller_mac, (const uint8_t*)&mask, sizeof(mask));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "📤 Отправлена маска Управляющему: %08" PRIX32, mask);
    } else {
        ESP_LOGE(TAG, "❌ Ошибка отправки маски: %d", ret);
    }
}

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (data == NULL || len <= 0) return;

    if (len == 4) {
        uint32_t temp;
        memcpy(&temp, data, 4);
        atomic_store(&lamp_state, temp);
        ESP_LOGI(TAG, "📥 Полный статус от Смотрящего: %08" PRIX32, temp);
    } else if (len == 2) {
        uint16_t old_state;
        memcpy(&old_state, data, 2);
        atomic_store(&lamp_state, (uint32_t)old_state);
        ESP_LOGI(TAG, "📥 Статус от Смотрящего (16 бит): %04X", old_state);
    } else {
        ESP_LOGW(TAG, "Неверная длина статуса: %d", len);
    }
}

static void init_espnow(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Защита от ухода с частоты: фиксируем канал связи
    ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    esp_now_peer_info_t peer_monitor = {
        .channel = WIFI_CHANNEL,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer_monitor.peer_addr, monitor_mac, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer_monitor));

    esp_now_peer_info_t peer_controller = {
        .channel = WIFI_CHANNEL,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer_controller.peer_addr, controller_mac, 6);
    if (esp_now_add_peer(&peer_controller) == ESP_OK) {
        ESP_LOGI(TAG, "✅ Peer Управляющего добавлен");
    } else {
        ESP_LOGE(TAG, "Ошибка добавления peer Управляющего");
    }
}

static void uart_task(void *pvParameters) {
    uint8_t data[BUF_SIZE];
    char line[BUF_SIZE];
    int pos = 0;

    while (1) {
        int len = uart_read_bytes(UART_GATEWAY_PORT, data, sizeof(data) - 1, pdMS_TO_TICKS(100));
        for (int i = 0; i < len; i++) {
            if (data[i] == '\n' || data[i] == '\r') {
                if (pos > 0) { // Игнорируем повторные символы \n после \r
                    line[pos] = '\0';
                    if (strncmp(line, "MASK ", 5) == 0) {
                        uint32_t mask = (uint32_t)strtoul(&line[5], NULL, 16);
                        ESP_LOGI(TAG, "📥 UART: получена маска %08" PRIX32, mask);
                        if (xQueueSend(mask_queue, &mask, pdMS_TO_TICKS(50)) != pdTRUE) {
                            ESP_LOGW(TAG, "⚠️ Очередь переполнена");
                        }
                    } else {
                        ESP_LOGW(TAG, "Неизвестный пакет: %s", line);
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

static void init_uart(void) {
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_GATEWAY_PORT, &uart_config);
    uart_set_pin(UART_GATEWAY_PORT, UART_GATEWAY_TXD, UART_GATEWAY_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_GATEWAY_PORT, BUF_SIZE, BUF_SIZE, 0, NULL, 0);
    ESP_LOGI(TAG, "✅ UART инициализирован");
}

static void command_task(void *pvParameters) {
    uint32_t requested_mask = 0;

    while (1) {
        if (xQueueReceive(mask_queue, &requested_mask, portMAX_DELAY) == pdTRUE) {
            uint32_t current_state = atomic_load(&lamp_state); 
            uint32_t actual_mask = requested_mask & (~current_state);
            
            if (actual_mask != 0) {
                ESP_LOGI(TAG, "⚙️ Отправка импульса на реле. Количество: %d", __builtin_popcount(actual_mask));
                send_mask(actual_mask);
            } else {
                ESP_LOGI(TAG, "⚠️ Все лампы из маски %08" PRIX32 " уже включены", requested_mask);
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

    mask_queue = xQueueCreate(QUEUE_SIZE, sizeof(uint32_t));
    if (mask_queue == NULL) return;

    init_uart();
    init_espnow();

    xTaskCreate(uart_task, "uart_task", 3072, NULL, 5, NULL);
    xTaskCreate(command_task, "command_task", 2048, NULL, 5, NULL);
}
