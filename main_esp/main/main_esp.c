#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_mac.h"

static const char *TAG = "MAIN_ESP";

// MAC Смотрящего
static uint8_t monitor_mac[] = {0x44, 0x1D, 0x64, 0xF6, 0x91, 0x34};

// MAC Управляющего (пока закомментирован)
// static uint8_t controller_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint16_t lamp_state = 0x0000;

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len == sizeof(uint16_t)) {
        lamp_state = *(uint16_t*)data;
        ESP_LOGI(TAG, "Получен статус: %04X", lamp_state);
    }
}

void init_espnow(void) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));
    
    esp_now_peer_info_t peer = {
        .peer_addr = {monitor_mac[0], monitor_mac[1], monitor_mac[2],
                      monitor_mac[3], monitor_mac[4], monitor_mac[5]},
        .channel = 0,
        .ifidx = 0,           // ← исправлено
        .encrypt = false,
    };
    
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка добавления peer: %d", err);
    } else {
        ESP_LOGI(TAG, "Peer Смотрящего добавлен");
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    init_espnow();
    
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "Главный (ESP32-C6) запущен. MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}