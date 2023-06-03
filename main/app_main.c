#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

#define EX_UART_NUM    UART_NUM_2
#define POWER_GPIO_PIN GPIO_NUM_18
#define BUF_SIZE       1024
#define RD_BUF_SIZE    BUF_SIZE

#define MQTT_SERVER "mqtt.innoway.vn"
#define PORT        "1883"
#define DEVICE_ID   "129299aa-99a5-49c5-b758-e7f352e74801"
#define CLIENT_ID   "hungok"
#define USERNAME    "hungok"
#define PASSWORD    "MCSvgZLO56gyYTOK9a5EVCxbb1gsjWLe"
#define PUB_DATA                                                               \
    "{\"cell\":%d,\"pci\":%d,\"rsrp\":%d,\"rsrq\":%d,\"sinr\":%d,\"cellid\":%" \
    "d,\"longitude\":%f,\"latitude\":%f}\r\n"
#define AT_CMD_PUB_MQTT "AT+SMPUB=\"messages/" DEVICE_ID "/update\",%d,0,1\r\n"
#define CLBS_RES        "+CLBS: 0,106.642897,29.487558,500\n\nOK\n"
#define CENG_RES                                                               \
    "+CENG:1,1,3,LTE "                                                         \
    "NB-IOT\n\n+CENG:0,\"1791,322,-72,-60,-12,2,45119,152142613,452,04,"       \
    "255\"\n+CENG:1,\"1791,119,-80,-61,-17,2\"\n+CENG:2,\"1791,410,-81,-61,-"  \
    "19,2\"\n\nOK\n"

const char at_cmd_connect_mqtt[][100] = {
    "AT+CNACT=0,1\r\n",
    "AT+SMCONF=\"URL\",\"" MQTT_SERVER "\"," PORT "\r\n",
    "AT+SMCONF=\"CLIENTID\",\"" CLIENT_ID "\"\r\n",
    "AT+SMCONF=\"USERNAME\",\"" USERNAME "\"\r\n",
    "AT+SMCONF=\"PASSWORD\",\"" PASSWORD "\"\r\n",
    "AT+SMCONN\r\n"};

static const char *UART = "UART";
static const char *PRINTF = "PRINTF";
static QueueHandle_t uart2_queue;
static int tmp, cell, pci, rsrp, rsrq, sinr, cellid;
static float longitude, latitude;
static bool connect = false;

// Forward declarations
void handleDataCENG(const char *data);
void handleDataCLBS(const char *data);
void uart_event_task(void *pvParameters);
void send_at_cmd(const char *src);
void powerOnSetup();
void powerOn();
void powerOff();
void uart_setup();
bool connect_mqtt_broker();
void pub_mqtt_data();

void handleDataCENG(const char *data) {
    char str[BUF_SIZE];
    sscanf(data, "%[^:]:%[^:]: %d,\"%d, %d, %d, %d, %d, %d, %d, %d", str, str,
           &cell, &tmp, &pci, &rsrp, &tmp, &rsrq, &sinr, &tmp, &cellid);

    ESP_LOGI(PRINTF, "cell: %d", cell);
    ESP_LOGI(PRINTF, "pci: %d", pci);
    ESP_LOGI(PRINTF, "rsrp: %d", rsrp);
    ESP_LOGI(PRINTF, "rsrq: %d", rsrq);
    ESP_LOGI(PRINTF, "sinr: %d", sinr);
    ESP_LOGI(PRINTF, "cellid: %d", cellid);
}

void handleDataCLBS(const char *data) {
    sscanf(data, "+CLBS:%*d,%f,%f", &longitude, &latitude);

    ESP_LOGI(PRINTF, "longitude: %f", longitude);
    ESP_LOGI(PRINTF, "latitude: %f", latitude);
}

void uart_event_task(void *pvParameters) {
    uart_event_t event;
    uint8_t *dtmp = (uint8_t *) malloc(RD_BUF_SIZE);
    char res_buf[RD_BUF_SIZE] = "";
    size_t res_len = 0;
    int len;
    while (1) {
        if (xQueueReceive(uart2_queue, (void *) &event,
                          (TickType_t) portMAX_DELAY)) {
            bzero(dtmp, RD_BUF_SIZE);
            switch (event.type) {
                case UART_DATA:
                    len = uart_read_bytes(EX_UART_NUM, dtmp, event.size,
                                          portMAX_DELAY);
                    memcpy(res_buf + res_len, dtmp, len);
                    res_len += len;
                    if (strstr(res_buf, "+C") != NULL) {
                        if (strstr(res_buf, "OK") != NULL) {
                            ESP_LOGI("UART", "%s", res_buf);
                            if (strstr(res_buf, "CENG") != NULL) {
                                handleDataCENG(res_buf);
                            } else if (strstr(res_buf, "CLBS") != NULL) {
                                handleDataCLBS(res_buf);
                            }
                            res_len = 0;
                            memset(res_buf, 0, sizeof(res_buf));
                        }
                    } else {
                        if (connect) {
                            if (strstr(res_buf, "OK") != NULL) {
                                connect = false;
                            }
                            ESP_LOGI("UART", "%s", res_buf);
                            res_len = 0;
                            memset(res_buf, 0, sizeof(res_buf));
                        } else {
                            ESP_LOGI("UART", "%s", res_buf);
                            res_len = 0;
                            memset(res_buf, 0, sizeof(res_buf));
                        }
                    }
                    break;
                default:
                    ESP_LOGI(UART, "uart event type: %d", event.type);
                    break;
            }
        }
    }
    free(dtmp);
    dtmp = NULL;
    vTaskDelete(NULL);
}

void send_at_cmd(const char *src) {
    ESP_LOGI(PRINTF, "%s", src);
    uart_write_bytes(EX_UART_NUM, src, strlen(src));
    vTaskDelay(2000 / portTICK_PERIOD_MS);
}

void powerOnSetup() {
    gpio_config_t io_conf = {.intr_type = GPIO_INTR_DISABLE,
                             .mode = GPIO_MODE_OUTPUT,
                             .pin_bit_mask = (1ULL << POWER_GPIO_PIN),
                             .pull_down_en = GPIO_PULLDOWN_DISABLE,
                             .pull_up_en = GPIO_PULLUP_DISABLE};
    gpio_config(&io_conf);
    gpio_set_level(POWER_GPIO_PIN, 0);
}

void powerOn() {
    ESP_LOGI(PRINTF, "POWER ON");
    gpio_set_level(POWER_GPIO_PIN, 1);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    gpio_set_level(POWER_GPIO_PIN, 0);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}

void powerOff() {
    ESP_LOGI(PRINTF, "POWER OFF");
    send_at_cmd("AT+CPOWD=1\r\n");
}

void uart_setup() {
    uart_config_t uart_config = {.baud_rate = 115200,
                                 .data_bits = UART_DATA_8_BITS,
                                 .parity = UART_PARITY_DISABLE,
                                 .stop_bits = UART_STOP_BITS_1,
                                 .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                                 .source_clk = UART_SCLK_APB};

    uart_driver_install(EX_UART_NUM, BUF_SIZE * 4, BUF_SIZE * 4, 20,
                        &uart2_queue, 0);
    uart_param_config(EX_UART_NUM, &uart_config);

    uart_set_pin(EX_UART_NUM, 17, 16, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreate(uart_event_task, "uart_event_task", 8192, NULL, 12, NULL);
}

void pub_mqtt_data() {
    char public_data[RD_BUF_SIZE];
    snprintf(public_data, RD_BUF_SIZE, PUB_DATA, cell, pci, rsrp, rsrq, sinr,
             cellid, longitude, latitude);

    char public_mess[RD_BUF_SIZE];
    snprintf(public_mess, RD_BUF_SIZE, AT_CMD_PUB_MQTT, strlen(public_data));

    send_at_cmd(public_mess);
    send_at_cmd(public_data);
}

bool connect_mqtt_broker() {
    int count = 5;
    int len = sizeof(at_cmd_connect_mqtt) / sizeof(at_cmd_connect_mqtt[0]);
    for (int i = 0; i < len; i++) {
        if (i == len - 1) connect = true;
        while (connect && count != 0)
            send_at_cmd(at_cmd_connect_mqtt[i]);
        count--;
    }
    if (count == 0)
        return false;
    else
        return true;
}

void app_main(void) {
    powerOnSetup();
    uart_setup();
    int count;
    bool success;
    while (1) {
        powerOn();
        send_at_cmd("ATE0\r\n");
        // send_at_cmd(CENG_RES);
        // send_at_cmd(CLBS_RES);
        // send_at_cmd("AT+CLBS=1,0\r\n");
        count = 5;
        while (pci == 0 && count != 0) {
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            send_at_cmd("AT+CENG?\r\n");
            count--;
        }
        if (count != 0) {
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            success = connect_mqtt_broker();
            if (success) {
                pub_mqtt_data();
                vTaskDelay(5000 / portTICK_PERIOD_MS);
            }
        }
        powerOff();
        // vTaskDelay(300000 / portTICK_PERIOD_MS);
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}
