#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "uart_events";

#define EX_UART_NUM     UART_NUM_0
#define PATTERN_CHR_NUM (3)

#define BUF_SIZE    (1024)
#define RD_BUF_SIZE (BUF_SIZE)
static QueueHandle_t uart_queue;
static int uart_num;

static void uart_event_task(void *pvParameters) {
    uart_event_t event;
    size_t buffered_size;
    uint8_t *dtmp = (uint8_t *) malloc(RD_BUF_SIZE);
    for (;;) {
        // Waiting for UART event.
        if (xQueueReceive(uart_queue, (void *) &event,
                          (TickType_t) portMAX_DELAY)) {
            bzero(dtmp, RD_BUF_SIZE);
            ESP_LOGI(TAG, "uart[%d] event:", EX_UART_NUM);
            switch (event.type) {
                // Event of UART receving data
                case UART_DATA:
                    ESP_LOGI(TAG, "[UART DATA]: %d", event.size);
                    uart_read_bytes(EX_UART_NUM, dtmp, event.size,
                                    portMAX_DELAY);
                    ESP_LOGI(TAG, "[DATA EVT]:");
                    uart_write_bytes(EX_UART_NUM, (const char *) dtmp,
                                     event.size);
                    break;
                // Event of HW FIFO overflow detected
                case UART_FIFO_OVF:
                    ESP_LOGI(TAG, "hw fifo overflow");
                    uart_flush_input(EX_UART_NUM);
                    xQueueReset(uart_queue);
                    break;
                // Event of UART ring buffer full
                case UART_BUFFER_FULL:
                    ESP_LOGI(TAG, "ring buffer full");
                    uart_flush_input(EX_UART_NUM);
                    xQueueReset(uart_queue);
                    break;
                // Event of UART RX break detected
                case UART_BREAK:
                    ESP_LOGI(TAG, "uart rx break");
                    break;
                // Event of UART parity check error
                case UART_PARITY_ERR:
                    ESP_LOGI(TAG, "uart parity error");
                    break;
                // Event of UART frame error
                case UART_FRAME_ERR:
                    ESP_LOGI(TAG, "uart frame error");
                    break;
                // UART_PATTERN_DET
                case UART_PATTERN_DET:
                    uart_get_buffered_data_len(EX_UART_NUM, &buffered_size);
                    int pos = uart_pattern_pop_pos(EX_UART_NUM);
                    ESP_LOGI(
                        TAG,
                        "[UART PATTERN DETECTED] pos: %d, buffered size: %d",
                        pos, buffered_size);
                    if (pos == -1) {
                        uart_flush_input(EX_UART_NUM);
                    } else {
                        uart_read_bytes(EX_UART_NUM, dtmp, pos,
                                        100 / portTICK_PERIOD_MS);
                        uint8_t pat[PATTERN_CHR_NUM + 1];
                        memset(pat, 0, sizeof(pat));
                        uart_read_bytes(EX_UART_NUM, pat, PATTERN_CHR_NUM,
                                        100 / portTICK_PERIOD_MS);
                        ESP_LOGI(TAG, "read data: %s", dtmp);
                        ESP_LOGI(TAG, "read pat : %s", pat);
                    }
                    break;
                // Others
                default:
                    ESP_LOGI(TAG, "uart event type: %d", event.type);
                    break;
            }
        }
    }
    free(dtmp);
    dtmp = NULL;
    vTaskDelete(NULL);
}

void uart_setup(int uart_num_config, int uart_tx_pin, int uart_rx_pin,
                const uint32_t sizeOfStack) {
    uart_num = uart_num_config;

    esp_log_level_set(TAG, ESP_LOG_INFO);

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_driver_install(uart_num, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart_queue,
                        0);
    uart_param_config(uart_num, &uart_config);

    // Set UART log level
    esp_log_level_set(TAG, ESP_LOG_INFO);
    // Set UART pins (using UART0 default pins ie no changes.)
    uart_set_pin(uart_num, uart_tx_pin, uart_rx_pin, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE);

    // Set uart pattern detect function.
    uart_enable_pattern_det_baud_intr(uart_num, '+', PATTERN_CHR_NUM, 9, 0, 0);
    // Reset the pattern queue length to record at most 20 pattern positions.
    uart_pattern_queue_reset(uart_num, 20);

    // Create a task to handler UART event from ISR
    xTaskCreate(uart_event_task, "uart_event_task", sizeOfStack, NULL, 12,
                NULL);
}
