#pragma once

void uart_init(void);
void uart_send_command(const char *cmd);
void uart_start_rx_task(void);