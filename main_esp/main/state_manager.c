#include <stdio.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "state_manager.h"
#include "espnow_handler.h"
#include "uart_handler.h"
#include <inttypes.h>

static const char *TAG = "STATE_MANAGER";
static _Atomic uint32_t current_state = ATOMIC_VAR_INIT(0x00000000);

// Семафор для синхронизации отправки запроса и получения ответа по радиоканалу
static SemaphoreHandle_t state_ready_sem = NULL;

void state_manager_init(void) {
    state_ready_sem = xSemaphoreCreateBinary();
    if (state_ready_sem == NULL) {
        ESP_LOGE(TAG, "❌ Ошибка создания семафора синхронизации");
    }
    ESP_LOGI(TAG, "✅ State Manager инициализирован");
}

void state_manager_update_state(uint32_t new_state) {
    atomic_store(&current_state, new_state);
    ESP_LOGI(TAG, "📊 Состояние обновлено: %08" PRIX32, new_state);
    
    if (state_ready_sem != NULL) {
        xSemaphoreGive(state_ready_sem);
    }
}

uint32_t state_manager_get_state(void) {
    return atomic_load(&current_state);
}

bool state_manager_request_state(void) {
    if (!espnow_request_state()) {
        return false;
    }
    // Блокируем поток и ждем, пока коллбэк приема ESP-NOW выдаст семафор
    if (xSemaphoreTake(state_ready_sem, pdMS_TO_TICKS(300)) == pdTRUE) {
        return true;
    }
    ESP_LOGW(TAG, "⏰ Таймаут ответа от Смотрящего (300 мс)");
    return false;
}

void state_manager_execute(uint32_t mask, bool set) {
    if (mask == 0) {
        ESP_LOGW(TAG, "⚠️ Пустая маска, отправка отменена");
        return;
    }
    ESP_LOGI(TAG, "💡 %s: %08" PRIX32, set ? "Включение" : "Выключение", mask);
    espnow_send_to_controller(mask, set);
}

void state_manager_send_status_to_matter(void) {
    uint32_t state = atomic_load(&current_state);
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "STATE %08" PRIX32, state);
    uart_send_to_matter(buffer);
}

void state_manager_process_command(parsed_cmd_t cmd) {
    uint32_t need_mask = 0;
    bool is_set = false;
    
    switch (cmd.type) {
        case CMD_MASK_SET: need_mask = cmd.mask; is_set = true; break;
        case CMD_MASK_CLR: need_mask = cmd.mask; is_set = false; break;
        case CMD_ON: need_mask = 1UL << (cmd.lamp_num - 1); is_set = true; break;
        case CMD_OFF: need_mask = 1UL << (cmd.lamp_num - 1); is_set = false; break;
        case CMD_ALL_ON: need_mask = 0xFFFFFFFF; is_set = true; break;
        case CMD_ALL_OFF: need_mask = 0xFFFFFFFF; is_set = false; break;
        case CMD_STATUS:
            if (state_manager_request_state()) {
                uint32_t state = atomic_load(&current_state);
                ESP_LOGI(TAG, "📊 Текущее состояние: %08" PRIX32, state);
                state_manager_send_status_to_matter();
            } else {
                ESP_LOGW(TAG, "⚠️ Статус запросить не удалось");
            }
            return;
        default: return;
    }
    
    ESP_LOGI(TAG, "🔍 Запрос состояния перед изменением...");
    if (state_manager_request_state()) {
        uint32_t current = atomic_load(&current_state);
        uint32_t diff = is_set ? (need_mask & ~current) : (need_mask & current);
        if (diff != 0) {
            ESP_LOGI(TAG, "🎯 Требуется изменить: %08" PRIX32 " -> %s", diff, is_set ? "ON" : "OFF");
            state_manager_execute(diff, is_set);
        } else {
            ESP_LOGI(TAG, "✅ Устройства уже в целевом состоянии");
        }
    } else {
        ESP_LOGW(TAG, "⚠️ АВАРИЙНЫЙ РЕЖИМ: Смотрящий недоступен. Прямая слепая команда.");
        state_manager_execute(need_mask, is_set);
    }
}