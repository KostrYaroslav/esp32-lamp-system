#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "command_parser.h"

void state_manager_init(void);
void state_manager_update_state(uint32_t new_state);
uint32_t state_manager_get_state(void);
bool state_manager_request_state(void);  
void state_manager_execute(uint32_t mask, bool set);  
void state_manager_process_command(parsed_cmd_t cmd);
void state_manager_send_status_to_matter(void);