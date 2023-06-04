#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <gpio_lib.h>
#include <string.h>
#include <uart_lib.h>

#define EX_UART_NUM    UART_NUM_2
#define POWER_GPIO_PIN GPIO_NUM_18
#define SUCCESS_BIT    (1 << 0)

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

char at_cmd_connect_mqtt[][100] = {
    "AT+CNACT=0,1\r\n",
    "AT+SMCONF=\"URL\",\"" MQTT_SERVER "\"," PORT "\r\n",
    "AT+SMCONF=\"CLIENTID\",\"" CLIENT_ID "\"\r\n",
    "AT+SMCONF=\"USERNAME\",\"" USERNAME "\"\r\n",
    "AT+SMCONF=\"PASSWORD\",\"" PASSWORD "\"\r\n",
    "AT+SMCONN\r\n",
};

static EventGroupHandle_t xEventGroup;
static const char *PRINTF = "AT_command";
static int tmp, cell, pci, rsrp, rsrq, sinr, cellid;
static float longitude, latitude;

void handleDataCENG(const char *data) {
    char str[1024];
    sscanf(data, "%[^:]:%[^:]: %d,\"%d, %d, %d, %d, %d, %d, %d, %d", str, str,
           &cell, &tmp, &pci, &rsrp, &tmp, &rsrq, &sinr, &tmp, &cellid);
}

void powerOnSetup() {
    create_io(POWER_GPIO_PIN, GPIO_MODE_OUTPUT, GPIO_INTR_DISABLE);
    gpio_set_level(POWER_GPIO_PIN, 0);
}

void powerOn() {
    ESP_LOGI(PRINTF, "Power on");
    gpio_set_level(POWER_GPIO_PIN, 1);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    gpio_set_level(POWER_GPIO_PIN, 0);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}

void powerOff() {
    ESP_LOGI(PRINTF, "Power off");
    uart_send_data("AT+CPOWD=1\r\n");
}

void pub_mqtt_data() {
    char public_data[1024];
    snprintf(public_data, 1024, PUB_DATA, cell, pci, rsrp, rsrq, sinr, cellid,
             longitude, latitude);

    char public_mess[1024];
    snprintf(public_mess, 1024, AT_CMD_PUB_MQTT, strlen(public_data));

    uart_send_data(public_mess);
    uart_send_data(public_data);
}

void connect_mqtt_broker() {
    int len = sizeof(at_cmd_connect_mqtt) / sizeof(at_cmd_connect_mqtt[0]);
    for (int i = 0; i < len; i++) {
        uart_send_data(at_cmd_connect_mqtt[i]);
    }
}

void app_main(void) {
    powerOnSetup();
    uart_setup(UART_NUM_2, GPIO_NUM_17, GPIO_NUM_16, 2048);
    xEventGroup = xEventGroupCreate();

    EventBits_t uxBits;
    while (1) {
        powerOn();
        uxBits = xEventGroupWaitBits(xEventGroup, SUCCESS_BIT, pdTRUE, pdFALSE,
                                     portMAX_DELAY);
        if (uxBits & SUCCESS_BIT) {
            uart_send_data();
        }
    }
    uart_send_data("ATE0\r\n");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    uart_send_data("AT+CENG?\r\n");
    vTaskDelay(5000 / portTICK_PERIOD_MS);
    pub_mqtt_data();
    vTaskDelay(5000 / portTICK_PERIOD_MS);
    powerOff();
    vTaskDelay(5000 / portTICK_PERIOD_MS);
}
