#include <stdio.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "state_manager.h"
#include "uart_handler.h"

static const char *TAG = "STATE_MANAGER";

static _Atomic uint32_t current_state = ATOMIC_VAR_INIT(0x00010005); // лампы 1,3,17
static bool has_state = true;

void state_manager_init(void) {
    ESP_LOGI(TAG, "✅ State Manager инициализирован");
}

void state_manager_update(uint32_t new_state) {
    atomic_store(&current_state, new_state);
    has_state = true;
    ESP_LOGI(TAG, "📊 Состояние ламп обновлено: %08" PRIX32, new_state);
}

uint32_t state_manager_get_current(void) {
    return atomic_load(&current_state);
}

bool state_manager_is_valid(void) {
    return has_state;
}

uint32_t state_manager_calculate_diff(uint32_t target, bool set) {
    uint32_t current = atomic_load(&current_state);
    if (set) {
        // Включить: только выключенные
        return target & (~current);
    } else {
        // Выключить: только включённые
        return target & current;
    }
}

void state_manager_execute_command(uint32_t mask, bool is_set) {
    if (mask == 0) {
        ESP_LOGW(TAG, "⚠️ Пустая маска, команда не выполнена");
        return;
    }
    
    if (is_set) {
        ESP_LOGI(TAG, "💡 Включение ламп по маске: %08" PRIX32, mask);
        char buffer[48];
        snprintf(buffer, sizeof(buffer), "MASK_SET %08" PRIX32, mask);
        uart_send_command(buffer);
    } else {
        ESP_LOGI(TAG, "💡 Выключение ламп по маске: %08" PRIX32, mask);
        char buffer[48];
        snprintf(buffer, sizeof(buffer), "MASK_CLR %08" PRIX32, mask);
        uart_send_command(buffer);
    }
}