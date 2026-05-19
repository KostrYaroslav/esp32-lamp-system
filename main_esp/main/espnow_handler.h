#pragma once
#include <stdint.h>
#include <stdbool.h>

void espnow_init(void);
void espnow_send_to_controller(uint32_t mask, bool set);
bool espnow_request_state(void);