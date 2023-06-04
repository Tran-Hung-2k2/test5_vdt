#ifndef UART_LIB_H
#define UART_LIB_H

typedef void (*data_event_callback_t)(char *, int);

void uart_set_callback(void *cb);
void uart_setup(int uart_num_config, int uart_tx_pin, int uart_rx_pin,
                const uint32_t sizeOfStack);
void uart_send_data(char *data);

#endif
