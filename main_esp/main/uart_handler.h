#pragma once

void uart_init(void);
void uart_send_to_matter(const char *msg);
void uart_start_terminal_task(void);
void uart_start_matter_rx_task(void);