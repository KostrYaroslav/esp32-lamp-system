#pragma once

#include <stdio.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "state_manager.h"
#include "uart_handler.h"

void state_manager_init(void);
void state_manager_update(uint32_t new_state);
uint32_t state_manager_get_current(void);
bool state_manager_is_valid(void);
uint32_t state_manager_calculate_diff(uint32_t target, bool set);
void state_manager_execute_command(uint32_t mask, bool is_set);