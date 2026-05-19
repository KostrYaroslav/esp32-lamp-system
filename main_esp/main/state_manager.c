#include <stdio.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
// Мьютекс для предотвращения гонки между параллельными запросами статуса
static SemaphoreHandle_t state_lock_mutex = NULL;

void state_manager_init(void) {
    state_ready_sem = xSemaphoreCreateBinary();
    state_lock_mutex = xSemaphoreCreateMutex();
    
    if (state_ready_sem == NULL || state_lock_mutex == NULL) {
        ESP_LOGE(TAG, "❌ Критическая ошибка создания объектов синхронизации");
        return;
    }
    ESP_LOGI(TAG, "✅ State Manager успешно инициализирован (Мьютекс + Семафор)");
}

void state_manager_update_state(uint32_t new_state) {
    atomic_store(&current_state, new_state);
    ESP_LOGI(TAG, "📊 Состояние обновлено: %08" PRIX32, new_state);
    
    if (state_ready_sem != NULL) {
        // Безопасная выгрузка семафора с проверкой контекста прерывания
        if (xPortInIsrContext()) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xSemaphoreGiveFromISR(state_ready_sem, &xHigherPriorityTaskWoken);
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        } else {
            xSemaphoreGive(state_ready_sem);
        }
    }
}

uint32_t state_manager_get_state(void) {
    return atomic_load(&current_state);
}

bool state_manager_request_state(void) {
    if (state_ready_sem == NULL || state_lock_mutex == NULL) return false;

    // Захватываем мьютекс монопольного доступа к транзакции опроса
    if (xSemaphoreTake(state_lock_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "⚠️ Ресурс опроса заблокирован другим потоком");
        return false;
    }

    // ФИКС ГОНКИ: Сбрасываем семафор перед запросом, чтобы убрать "фантомные" старые сигналы
    xSemaphoreTake(state_ready_sem, 0); 

    if (!espnow_request_state()) {
        xSemaphoreGive(state_lock_mutex);
        return false;
    }
    
    // Блокируем поток и ждем, пока коллбэк приема ESP-NOW выдаст семафор
    bool success = false;
    if (xSemaphoreTake(state_ready_sem, pdMS_TO_TICKS(300)) == pdTRUE) {
        success = true;
    } else {
        ESP_LOGW(TAG, "⏰ Таймаут ответа от Смотрящего (300 мс)");
    }
    
    xSemaphoreGive(state_lock_mutex);
    return success;
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
    
    // Шаг 1: Парсинг типа входящей команды и формирование целевой маски
    switch (cmd.type) {
        case CMD_MASK_SET: need_mask = cmd.mask; is_set = true; break;
        case CMD_MASK_CLR: need_mask = cmd.mask; is_set = false; break;
        case CMD_ON:  need_mask = 1UL << (cmd.lamp_num - 1); is_set = true; break;
        case CMD_OFF: need_mask = 1UL << (cmd.lamp_num - 1); is_set = false; break;
        case CMD_ALL_ON:  need_mask = 0xFFFFFFFF; is_set = true; break;
        case CMD_ALL_OFF: need_mask = 0xFFFFFFFF; is_set = false; break;
        case CMD_STATUS:
            if (state_manager_request_state()) {
                uint32_t state = atomic_load(&current_state);
                ESP_LOGI(TAG, "📊 Текущее актуальное состояние: %08" PRIX32, state);
                state_manager_send_status_to_matter();
            } else {
                ESP_LOGW(TAG, "⚠️ Статус запросить не удалось. Отправка кэша.");
                state_manager_send_status_to_matter(); 
            }
            return;
        default: return;
    }
    
    ESP_LOGI(TAG, "🔍 Запрос состояния «Смотрящего» перед изменением...");
    
    // Шаг 2: Монополизируем мьютекс на весь цикл транзакции опроса и отправки
    if (xSemaphoreTake(state_lock_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        
        // Очищаем старые сигналы перед отправкой радиопакета
        xSemaphoreTake(state_ready_sem, 0); 

        // Запрашиваем состояние напрямую через драйвер радиоканала (защита от повторного захвата мьютекса)
        if (espnow_request_state() && xSemaphoreTake(state_ready_sem, pdMS_TO_TICKS(300)) == pdTRUE) {
            
            uint32_t current = atomic_load(&current_state);
            uint32_t diff = 0;

            // Шаг 3: Вычисление дифференциальной маски импульса
            if (is_set) {
                // Команда ВКЛЮЧИТЬ: берем лампы, которые СЕЙЧАС ВЫКЛЮЧЕНЫ
                diff = need_mask & ~current;
            } else {
                // Команда ВЫКЛЮЧИТЬ: берем лампы, которые СЕЙЧАС ВКЛЮЧЕНЫ
                diff = need_mask & current;
            }
            
            // Шаг 4: Подача выборочного импульса
            if (diff != 0) {
                ESP_LOGI(TAG, "🎯 Текущее состояние: %08" PRIX32 ". Отправка диф-импульса: %08" PRIX32 " -> %s", 
                         current, diff, is_set ? "ON" : "OFF");
                state_manager_execute(diff, is_set);
            } else {
                ESP_LOGI(TAG, "✅ Отмена импульса: все целевые лампы из маски уже находятся в состоянии %s", 
                         is_set ? "ON" : "OFF");
            }
        } else {
            // Аварийный сценарий: если Смотрящий не отвечает, бьем «вслепую» по полной маске запроса
            ESP_LOGW(TAG, "⚠️ АВАРИЙНЫЙ РЕЖИМ: Смотрящий не ответил. Слепой импульс на всю маску.");
            state_manager_execute(need_mask, is_set);
        }
        
        // Освобождаем мьютекс шины управления
        xSemaphoreGive(state_lock_mutex);
    } else {
        ESP_LOGE(TAG, "🚨 Команда отклонена: шина управления занята параллельным процессом");
    }
}
