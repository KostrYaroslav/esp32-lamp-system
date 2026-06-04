#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "driver/uart.h"
#include "espnow_handler.h"
#include "state_manager.h"

static const char *TAG = "ESPNOW";
static const uint8_t monitor_mac[6] = {0x44, 0x1D, 0x64, 0xF6, 0x91, 0x34};
static const uint8_t controller_mac[6] = {0x68, 0xFE, 0x71, 0x88, 0x8E, 0x48};

#define WIFI_CHANNEL 1
#define CMD_GET_STATE 0x01
#define MATTER_UART_NUM UART_NUM_1

static SemaphoreHandle_t state_semaphore = NULL;
static SemaphoreHandle_t dialog_sem = NULL;
static volatile bool waiting = false;
static _Atomic uint32_t last_response_mask = ATOMIC_VAR_INIT(0);

typedef struct {
    uint32_t mask;
    uint8_t action;
} __attribute__((packed)) controller_pkt_t;

typedef struct {
    uint8_t step;
    uint32_t mask;
} __attribute__((packed)) response_pkt_t;

static void handle_received_state(uint32_t state) {
    state_manager_update_state(state);
    char uart_buf[32];
    snprintf(uart_buf, sizeof(uart_buf), "Lamp Mask: %08X\r\n", (unsigned int)state);
    uart_write_bytes(MATTER_UART_NUM, uart_buf, strlen(uart_buf));
    ESP_LOGI(TAG, "🚀 Маска отправлена в UART: %08X", (unsigned int)state);
    
    if (waiting) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(state_semaphore, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

// Универсальный коллбэк для приема данных (Исправлен на FromISR!)
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!info) return;
    const uint8_t *src_mac = info->src_addr;
#else
static void on_data_recv(const uint8_t *mac_addr, const uint8_t *data, int len) {
    if (!mac_addr) return;
    const uint8_t *src_mac = mac_addr;
#endif

    // От Смотрящего (4 байта состояния)
    if (memcmp(src_mac, monitor_mac, 6) == 0 && len == 4) {
        uint32_t state;
        memcpy(&state, data, 4);
        handle_received_state(state);
        return;
    }

    // Ответы от Управляющего (Переписано строго через FromISR!)
    if (memcmp(src_mac, controller_mac, 6) == 0 && len == sizeof(response_pkt_t)) {
        response_pkt_t resp;
        memcpy(&resp, data, sizeof(resp));
        
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        if (resp.step == 0x02 || resp.step == 0x04) {
            xSemaphoreGiveFromISR(dialog_sem, &xHigherPriorityTaskWoken);
        } else if (resp.step == 0x03) {
            atomic_store(&last_response_mask, resp.mask);
            xSemaphoreGiveFromISR(dialog_sem, &xHigherPriorityTaskWoken);
        }
        
        if (xHigherPriorityTaskWoken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
        return;
    }
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "⚠️ Сбой доставки пакета");
    }
}
#else
static void on_data_sent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "⚠️ Сбой доставки пакета");
    }
}
#endif

void espnow_init(void) {
    state_semaphore = xSemaphoreCreateBinary();
    dialog_sem = xSemaphoreCreateBinary();
    if (state_semaphore == NULL || dialog_sem == NULL) {
        ESP_LOGE(TAG, "❌ Не удалось создать семафоры");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    
    // ОБЯЗАТЕЛЬНО для ESP32-C6: создаем интерфейс
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));
    
    ESP_ERROR_CHECK(esp_now_init());
    
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));
#else
    ESP_ERROR_CHECK(esp_now_register_recv_cb((esp_now_recv_cb_t)on_data_recv));
#endif
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));

    esp_now_peer_info_t peer = { .channel = WIFI_CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = false };
    
    memcpy(peer.peer_addr, monitor_mac, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    
    memcpy(peer.peer_addr, controller_mac, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    
    ESP_LOGI(TAG, "✅ ESP-NOW успешно запущен на ESP32-C6");
}

void espnow_send_to_controller(uint32_t mask, bool set) {
    controller_pkt_t pkt = { .mask = mask, .action = set ? 1 : 0 };
    esp_now_send((uint8_t*)controller_mac, (uint8_t*)&pkt, sizeof(pkt));
}

bool espnow_request_state(void) {
    xSemaphoreTake(state_semaphore, 0);
    waiting = true;
    uint8_t cmd = CMD_GET_STATE;
    esp_err_t err = esp_now_send((uint8_t*)monitor_mac, &cmd, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Не удалось отправить запрос Смотрящему: %d", err);
        waiting = false;
        return false;
    }
    bool ok = (xSemaphoreTake(state_semaphore, pdMS_TO_TICKS(150)) == pdTRUE);
    waiting = false;
    return ok;
}

bool espnow_dialog_with_controller(uint32_t mask, bool set) {
    if (dialog_sem == NULL) return false;

    // Шаг 1: PING
    uint8_t ping = 0x01;
    xSemaphoreTake(dialog_sem, 0); // Очищаем семафор перед транзакцией
    esp_err_t err = esp_now_send((uint8_t*)controller_mac, &ping, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ PING не отправлен");
        return false;
    }
    ESP_LOGI(TAG, "🔍 PING отправлен Управляющему");
    if (xSemaphoreTake(dialog_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        // Повторная очистка семафора на случай запоздавшего ответа
        return false;
    }
    ESP_LOGI(TAG, "✅ Управляющий на связи");

    // Шаг 2: EXEC
    controller_pkt_t cmd = { .mask = mask, .action = set ? 0x01 : 0x00 };
    err = esp_now_send((uint8_t*)controller_mac, (uint8_t*)&cmd, sizeof(cmd));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ EXEC не отправлен");
        return false;
    }
    ESP_LOGI(TAG, "📤 EXEC: маска %08" PRIX32 " (%s)", mask, set ? "ON" : "OFF");
    if (xSemaphoreTake(dialog_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "⚠️ Нет ECHO от Управляющего");
        return false;
    }
    
    uint32_t echoed_mask = atomic_load(&last_response_mask);
    if (echoed_mask != mask) {
        ESP_LOGW(TAG, "⚠️ Управляющий подтвердил другую маску: %08" PRIX32, echoed_mask);
        return false;
    }
    ESP_LOGI(TAG, "✅ Управляющий подтвердил: маска %08" PRIX32, echoed_mask);

    // Шаг 3: CONFIRM
    controller_pkt_t confirm = { .mask = mask, .action = 0xFF };
    err = esp_now_send((uint8_t*)controller_mac, (uint8_t*)&confirm, sizeof(confirm));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ CONFIRM не отправлен");
        return false;
    }
    ESP_LOGI(TAG, "📤 CONFIRM: маска %08" PRIX32, mask);
    if (xSemaphoreTake(dialog_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "⚠️ Нет DONE от Управляющего");
        return false;
    }
    
    ESP_LOGI(TAG, "✅ Управляющий выполнил команду");
    return true;
}
