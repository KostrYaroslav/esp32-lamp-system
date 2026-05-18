#pragma once

#include <stdbool.h>
#include <stdint.h>

void uart_init(void);
void uart_start_rx_task(void);
void uart_send_command(const char *cmd);
bool uart_request_state(void);