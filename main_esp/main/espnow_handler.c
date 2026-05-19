#include <string.h>
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
#include "espnow_handler.h"
#include "state_manager.h"

static const char *TAG = "ESPNOW";

static const uint8_t monitor_mac[6] = {0x44, 0x1D, 0x64, 0xF6, 0x91, 0x34};
static const uint8_t controller_mac[6] = {0x68, 0xFE, 0x71, 0x88, 0x8E, 0x48};

#define WIFI_CHANNEL 1
#define CMD_GET_STATE 0x01

static SemaphoreHandle_t state_semaphore = NULL;
static volatile bool waiting = false; 

typedef struct {
    uint32_t mask;
    uint8_t action;
} __attribute__((packed)) controller_pkt_t;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!info || memcmp(info->src_addr, monitor_mac, 6) != 0) return;
#else
static void on_data_recv(const uint8_t *mac_addr, const uint8_t *data, int len) {
    if (!mac_addr || memcmp(mac_addr, monitor_mac, 6) != 0) return;
#endif
    if (len == 4) {
        uint32_t state;
        memcpy(&state, data, 4);
        state_manager_update_state(state);
        
        if (waiting) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xSemaphoreGiveFromISR(state_semaphore, &xHigherPriorityTaskWoken);
            if (xHigherPriorityTaskWoken == pdTRUE) {
                portYIELD_FROM_ISR();
            }
        }
    }
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "⚠️ Сбой доставки пакета по ESP-NOW");
    }
}
#else
static void on_data_sent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "⚠️ Сбой доставки пакета по ESP-NOW");
    }
}
#endif

void espnow_init(void) {
    state_semaphore = xSemaphoreCreateBinary();
    if (state_semaphore == NULL) {
        ESP_LOGE(TAG, "❌ Не удалось создать семафор ESP-NOW");
        return;
    }
    
    ESP_ERROR_CHECK(esp_netif_init());
    
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Совместимость со старыми ESP32 (отключаем Wi-Fi 6)
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    
    ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));
    
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb((esp_now_recv_cb_t)on_data_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    
    esp_now_peer_info_t peer = { .channel = WIFI_CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = false };
    memcpy(peer.peer_addr, monitor_mac, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    
    memcpy(peer.peer_addr, controller_mac, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    
    ESP_LOGI(TAG, "✅ ESP-NOW успешно запущен (Режим совместимости Wi-Fi 4)");
}

void espnow_send_to_controller(uint32_t mask, bool set) {
    controller_pkt_t pkt = {
        .mask = mask,
        .action = set ? 1 : 0
    };
    
    esp_err_t err = esp_now_send(controller_mac, (uint8_t*)&pkt, sizeof(pkt));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Ошибка отправки управляющего пакета: %d", err);
    }
}

bool espnow_request_state(void) {
    xSemaphoreTake(state_semaphore, 0);
    waiting = true;
    uint8_t cmd = CMD_GET_STATE;
    
    esp_err_t err = esp_now_send(monitor_mac, &cmd, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Не удалось отправить запрос Смотрящему: %d", err);
        waiting = false;
        return false;
    }
    
    bool ok = (xSemaphoreTake(state_semaphore, pdMS_TO_TICKS(150)) == pdTRUE);
    waiting = false;
    return ok;
}