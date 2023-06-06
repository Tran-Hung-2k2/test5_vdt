#ifndef UART_LIB_H
#define UART_LIB_H

typedef void (*data_event_callback_t)(char *, int);

void uart_init(int uart_num_config, int uart_tx_pin, int uart_rx_pin,
               const uint32_t sizeOfStack, void *callback);
void uart_write_data(char *data, int len);
void uart_set_delay_before_read_data(int delay_ms);

#endif
