#ifndef UART_LIB_H
#define UART_LIB_H

void uart_setup(int uart_num_config, int uart_tx_pin, int uart_rx_pin,
                const uint32_t sizeOfStack);

#endif
