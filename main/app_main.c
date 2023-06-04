#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <freertos/event_groups.h>
#include <gpio_lib.h>
#include <string.h>
#include <uart_lib.h>

#define EX_UART_NUM        UART_NUM_2
#define POWER_GPIO_PIN     GPIO_NUM_18
#define CMD_SIZE           300
#define AT_CMD_SUCCESS_BIT (1 << 0)

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

static char at_cmd[][CMD_SIZE] = {
    "ATE0\r\n",
    "AT+CENG?\r\n",
    "AT+CNACT=0,1\r\n",
    "AT+SMCONF=\"URL\",\"" MQTT_SERVER "\"," PORT "\r\n",
    "AT+SMCONF=\"CLIENTID\",\"" CLIENT_ID "\"\r\n",
    "AT+SMCONF=\"USERNAME\",\"" USERNAME "\"\r\n",
    "AT+SMCONF=\"PASSWORD\",\"" PASSWORD "\"\r\n",
    "AT+SMCONN\r\n",
    "public_msg",
    "public_data",
    "AT+CPOWD=1\r\n",
};

static int delay_before_read[] = {
    2000, 2000, 2000, 2000, 2000, 2000, 2000, 2000, 2000, 2000, 2000,
};

static char response_success[][CMD_SIZE] = {
    "E0",     "CENG?",  "CNACT",    "SMCONF", "SMCONF", "SMCONF",
    "SMCONF", "SMCONN", "AT+SMPUB", "",       "AT",
};

static EventGroupHandle_t xEventGroup;
static EventBits_t uxBits;
static const char *LOG_AT_CMD = "SEND_AT_CMD";
static int tmp, cell = 0, pci = 0, rsrp = 0, rsrq = 0, sinr = 0, cellid = 0;
static float longitude = 0, latitude = 0;
static int current;  // Current AT_command send

void data_CENG_handler(const char *data) {
    char str[CMD_SIZE];
    sscanf(data, "%[^:]:%[^:]: %d,\"%d, %d, %d, %d, %d, %d, %d, %d", str, str,
           &cell, &tmp, &pci, &rsrp, &tmp, &rsrq, &sinr, &tmp, &cellid);

    char public_data[CMD_SIZE];
    snprintf(public_data, CMD_SIZE, PUB_DATA, cell, pci, rsrp, rsrq, sinr,
             cellid, longitude, latitude);

    char public_mess[CMD_SIZE];
    snprintf(public_mess, CMD_SIZE, AT_CMD_PUB_MQTT, strlen(public_data));

    memcpy(at_cmd[8], public_mess, strlen(public_mess));
    memcpy(at_cmd[9], public_data, strlen(public_data));
    if (strstr(public_data, "public_data") == NULL) {
        xEventGroupSetBits(xEventGroup, AT_CMD_SUCCESS_BIT);
        ESP_LOGI("Set_CENG_data", "SUCCESS");
    }
}

void data_uart_handler(char *data, int len) {
    ESP_LOGI("Receive_data", "%s", data);
    if (current == 1)
        data_CENG_handler(data);
    else if (strstr(data, response_success[current]))
        xEventGroupSetBits(xEventGroup, AT_CMD_SUCCESS_BIT);
}

void powerOnSetup() {
    create_io(POWER_GPIO_PIN, GPIO_MODE_OUTPUT, GPIO_INTR_DISABLE);
    gpio_set_level(POWER_GPIO_PIN, 0);
}

void powerOn() {
    gpio_set_level(POWER_GPIO_PIN, 1);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    gpio_set_level(POWER_GPIO_PIN, 0);
    ESP_LOGI(LOG_AT_CMD, "Power on");
    vTaskDelay(10 / portTICK_PERIOD_MS);
}

bool send_at_handler(char *command, int current_at_cmd, int resend_count,
                     int delay_before_read) {
    int current = current_at_cmd;
    int count = resend_count;
    uart_set_delay_before_read_data(delay_before_read);
    while (current == current_at_cmd && count > 0) {
        ESP_LOGI("Send_command", "%s", command);
        uart_write_data(command);
        uxBits = xEventGroupWaitBits(xEventGroup, AT_CMD_SUCCESS_BIT, pdTRUE,
                                     pdFALSE,
                                     pdMS_TO_TICKS(delay_before_read + 1000));
        if (uxBits & AT_CMD_SUCCESS_BIT) {
            current++;
        } else {
            count--;
        }
    }
    if (count <= 0)
        return false;
    else
        return true;
}

void app_main(void) {
    powerOnSetup();
    uart_setup(UART_NUM_2, GPIO_NUM_17, GPIO_NUM_16, 8192, data_uart_handler);
    xEventGroup = xEventGroupCreate();
    int at_cmd_array_len = sizeof(at_cmd) / sizeof(at_cmd[0]);
    bool isSuccess;
    while (1) {
        powerOn();
        for (current = 0; current < at_cmd_array_len; current++) {
            isSuccess = send_at_handler(at_cmd[current], current, 5,
                                        delay_before_read[current]);
            if (!isSuccess) break;
        }
        current = at_cmd_array_len - 1;
        isSuccess = send_at_handler(at_cmd[current], current, 5,
                                    delay_before_read[current]);
        if (isSuccess) {
            ESP_LOGI(LOG_AT_CMD, "Power off");
        } else {
            ESP_LOGI(LOG_AT_CMD, "Power off FAILURE");
        }
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
}
