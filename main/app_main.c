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
#define AT_CMD_FAILURE_BIT (1 << 1)

#define MQTT_SERVER "mqtt.innoway.vn"
#define PORT        "1883"
#define DEVICE_ID   "129299aa-99a5-49c5-b758-e7f352e74801"
#define CLIENT_ID   "hungok"
#define USERNAME    "hungok"
#define PASSWORD    "MCSvgZLO56gyYTOK9a5EVCxbb1gsjWLe"
#define PUB_DATA                                                               \
    "{\"pci\":%d,\"rsrp\":%d,\"rsrq\":%d,\"sinr\":%d,\"cellid\":%"             \
    "d,\"longitude\":%f,\"latitude\":%f}\r\n"
#define AT_CMD_PUB_MQTT "AT+SMPUB=\"messages/" DEVICE_ID "/update\",%d,0,1\r\n"

static EventGroupHandle_t xEventGroup;
static EventBits_t uxBits;
static int tmp, pci = 0, rsrp = 0, rsrq = 0, sinr = 0, cellid = 0;
static float longitude = 0, latitude = 0;
static int cur_cmd_id;  // Current AT_command send
static bool isSuccess;
static bool while_send = false;
static bool is_resend = false;
static char recv_data[CMD_SIZE];
static int len;
struct AT_command {
    char command[CMD_SIZE];
    char res_success[CMD_SIZE];
    int timeout;
};

struct AT_command at_cmd[] = {
    {"ATE0\r\n", "OK", 10000},
    {"AT+CENG?\r\n", "LTE NB-IOT", 20000},
    {"AT+CNACT=0,1\r\n", "OK", 15000},
    {"AT+CLBS=1,0\r\n", "CLBS: 0", 20000},
    {"AT+SMCONF=\"URL\",\"" MQTT_SERVER "\"," PORT "\r\n", "OK", 10000},
    {"AT+SMCONF=\"CLIENTID\",\"" CLIENT_ID "\"\r\n", "OK", 10000},
    {"AT+SMCONF=\"USERNAME\",\"" USERNAME "\"\r\n", "OK", 10000},
    {"AT+SMCONF=\"PASSWORD\",\"" PASSWORD "\"\r\n", "OK", 10000},
    {"AT+SMCONN\r\n", "OK", 20000},
    {"public_msg\r\n", "", 10000},
    {"public_data\r\n", "OK", 10000},
    {"AT+CPOWD=1\r\n", "NORMAL", 10000},
};

void data_CENG_handler(const char *data) {
    char str[CMD_SIZE];
    sscanf(data, "%[^\"]\"%d, %d, %d, %d, %d, %d, %d, %d", str, &tmp, &pci,
           &rsrp, &tmp, &rsrq, &sinr, &tmp, &cellid);

    ESP_LOGI("Set_CENG_data", "SUCCESS");
    xEventGroupSetBits(xEventGroup, AT_CMD_SUCCESS_BIT);
}

void data_CLBS_handler(const char *data) {
    char str[CMD_SIZE];
    sscanf(data, "%[^,],%f,%f", str, &longitude, &latitude);

    char public_data[CMD_SIZE];
    snprintf(public_data, CMD_SIZE, PUB_DATA, pci, rsrp, rsrq, sinr, cellid,
             longitude, latitude);

    char public_mess[CMD_SIZE];
    snprintf(public_mess, CMD_SIZE, AT_CMD_PUB_MQTT, strlen(public_data));

    memcpy(at_cmd[len - 3].command, public_mess, strlen(public_mess));
    memcpy(at_cmd[len - 2].command, public_data, strlen(public_data));
    ESP_LOGI("Set_CLBS_data", "SUCCESS");
    xEventGroupSetBits(xEventGroup, AT_CMD_SUCCESS_BIT);
}

void data_uart_handler(char *data, int len) {
    ESP_LOGI("Response", "%s", data);
    strcpy(recv_data, data);
    if (while_send) {
        if (strstr(data, at_cmd[cur_cmd_id].res_success) != NULL)
            xEventGroupSetBits(xEventGroup, AT_CMD_SUCCESS_BIT);
        else {
            ESP_LOGI("Fail_response", "%s", data);
            xEventGroupSetBits(xEventGroup, AT_CMD_FAILURE_BIT);
        }
    }
}

void powerOnSetup() {
    gpio_init_io(POWER_GPIO_PIN, GPIO_MODE_OUTPUT, GPIO_INTR_DISABLE);
    gpio_set_level(POWER_GPIO_PIN, 1);
}

void powerOn() {
    gpio_set_level(POWER_GPIO_PIN, 0);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    gpio_set_level(POWER_GPIO_PIN, 1);
    ESP_LOGI("STATUS", "Power on");
    vTaskDelay(20000 / portTICK_PERIOD_MS);
}

bool send_at_cmd(int cmd_id, int resend_count) {
    cur_cmd_id = cmd_id;
    while_send = true;
    int count = resend_count;
    while (cur_cmd_id == cmd_id && count > 0) {
        if (is_resend) {
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            ESP_LOGI("Resend", "%s", at_cmd[cmd_id].command);
        } else {
            ESP_LOGI("Send", "%s", at_cmd[cmd_id].command);
        }
        uart_write_data(at_cmd[cmd_id].command, strlen(at_cmd[cmd_id].command));
        uxBits = xEventGroupWaitBits(
            xEventGroup, AT_CMD_SUCCESS_BIT | AT_CMD_FAILURE_BIT, pdTRUE,
            pdFALSE, pdMS_TO_TICKS(at_cmd[cmd_id].timeout));
        if (uxBits & AT_CMD_SUCCESS_BIT) {
            if (strstr(at_cmd[cmd_id].command, "AT+CENG?") != NULL) {
                data_CENG_handler(recv_data);
            } else if (strstr(at_cmd[cmd_id].command, "AT+CLBS=1,0") != NULL) {
                is_resend = true;
                data_CLBS_handler(recv_data);
            }
            cur_cmd_id++;
        } else {
            count--;
        }
    }
    is_resend = false;
    while_send = false;
    if (count <= 0)
        return false;
    else
        return true;
}

void app_main(void) {
    uart_init(EX_UART_NUM, GPIO_NUM_17, GPIO_NUM_16, 2048, data_uart_handler);
    powerOnSetup();
    xEventGroup = xEventGroupCreate();
    len = sizeof(at_cmd) / sizeof(at_cmd[0]);
    while (1) {
        powerOn();
        for (int id = 0; id < len; id++) {
            isSuccess = send_at_cmd(id, 10);
            if (!isSuccess) {
                ESP_LOGI("STATUS", "FAILURE");
                id = len - 2;
            }
            vTaskDelay(3000 / portTICK_PERIOD_MS);
        }

        vTaskDelay(50000 / portTICK_PERIOD_MS);
    }
}
