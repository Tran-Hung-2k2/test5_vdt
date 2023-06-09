#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <freertos/event_groups.h>
#include <string.h>

#include <gpio_lib.h>
#include <uart_lib.h>

// Định nghĩa UART, GPIO điều khiển nguồn, các event bit và kích thước cmd
#define EX_UART_NUM        UART_NUM_2
#define POWER_GPIO_PIN     GPIO_NUM_18
#define CMD_SIZE           300
#define AT_CMD_SUCCESS_BIT (1 << 0)
#define AT_CMD_FAILURE_BIT (1 << 1)

// Định nghĩa thông tin MQTT
#define MQTT_SERVER "broker.hivemq.com"
#define PORT        "1883"
#define DEVICE_ID   "647ef8476acc038a2885462d"
// #define MQTT_SERVER "mqtt.innoway.vn"
// #define PORT        "1883"
// #define DEVICE_ID   "b97f33f4-7a30-4d2b-a9cc-d53a5806c483"
#define CLIENT_ID "hungok"
#define USERNAME  "hungok"
#define PASSWORD  "MCSvgZLO56gyYTOK9a5EVCxbb1gsjWLe"

// Định dạng AT command để publish dữ liệu lên MQTT
#define AT_CMD_PUB_MQTT "AT+SMPUB=\"messages/" DEVICE_ID "/update\",%d,0,1\r\n"

// Định dạng chuỗi dữ liệu để gửi lên MQTT
#define PUB_DATA                                                               \
    "{\"pci\":%d,\"rsrp\":%d,\"rsrq\":%d,\"sinr\":%d,\"cellid\":%"             \
    "d,\"longitude\":%f,\"latitude\":%f}\r\n"

// Khai báo biến
static EventGroupHandle_t xEventGroup;
static EventBits_t uxBits;
static TickType_t start, finish;
static int tmp, pci = 0, rsrp = 0, rsrq = 0, sinr = 0, cellid = 0;
static float longitude = 0, latitude = 0;
static int cur_cmd_id;           // Lệnh AT command đang được gửi
static bool isSuccess;           // True cmd có được gửi thành công
static bool while_send = false;  // True khi đang trong quá trình gửi cmd
static bool is_resend = false;  // True khi đang trong quá trình gửi lại cmd
static char recv_data[CMD_SIZE];  // Phản hồi nhận được từ UART
static int len;                   // Độ dài của mảng struct at_cmd

// Cấu trúc AT command
struct AT_command {
    char command[CMD_SIZE];  // Nội dung lệnh cần gửi
    char res_success[CMD_SIZE];  // Chuỗi "con" trong phản hồi thể hiện command
                                 // được module thực hiện thành công
    int timeout;  // Thời gian tối đa đợi phản hồi từ module
    int resend;   // Số lần gửi lại tối đa
};

// Mảng chứa các AT command cần gửi
struct AT_command at_cmd[] = {
    {"ATE0\r\n", "OK", 500, 15},
    {"AT+CENG?\r\n", "LTE NB-IOT", 20000, 10},
    {"AT+CNACT=0,1\r\n", "OK", 15000, 3},
    {"AT+CLBS=1,0\r\n", "CLBS: 0", 20000, 15},
    {"AT+SMCONF=\"URL\",\"" MQTT_SERVER "\"," PORT "\r\n", "OK", 10000, 2},
    {"AT+SMCONF=\"CLIENTID\",\"" CLIENT_ID "\"\r\n", "OK", 10000, 2},
    {"AT+SMCONF=\"USERNAME\",\"" USERNAME "\"\r\n", "OK", 10000, 2},
    {"AT+SMCONF=\"PASSWORD\",\"" PASSWORD "\"\r\n", "OK", 10000, 2},
    {"AT+SMCONN\r\n", "OK", 20000, 3},
    {"AT+SMPUB=\"messages/" DEVICE_ID "/update\",0,0,1\r\n", "", 10000, 2},
    {"{publish_messages}\r\n", "OK", 10000, 2},
    {"AT+CPOWD=1\r\n", "NORMAL", 10000, 10},
};

// Xử lý dữ liệu từ AT+CENG?
void data_CENG_handler(const char *data);
// Xử lý dữ liệu từ AT+CLBS=1,0
void data_CLBS_handler(const char *data);
// Xử lý dữ liệu nhận được từ UART
void data_uart_handler(char *data, int len);
// Thiết lập chân bật nguồn module
void powerOnSetup();
// Bật nguồn module
void powerOn();
// Gửi AT command
bool send_at_cmd(int cmd_id);

void data_CENG_handler(const char *data) {
    char str[CMD_SIZE];
    // Tách dữ liệu từ phản hồi
    sscanf(data, "%[^\"]\"%d, %d, %d, %d, %d, %d, %d, %d", str, &tmp, &pci,
           &rsrp, &tmp, &rsrq, &sinr, &tmp, &cellid);
    // In ra thông báo khi xử lý xong
    ESP_LOGI("Set_CENG_data", "SUCCESS");
}

void data_CLBS_handler(const char *data) {
    char str[CMD_SIZE];
    // Tách dữ liệu từ phản hồi
    sscanf(data, "%[^,],%f,%f", str, &longitude, &latitude);

    // Ghép dữ liệu vào chuỗi dữ liệu đã định dạng để publish
    char public_data[CMD_SIZE];
    snprintf(public_data, CMD_SIZE, PUB_DATA, pci, rsrp, rsrq, sinr, cellid,
             longitude, latitude);

    // Ghép độ dài chuỗi dữ liệu vào lệnh AT+SMPUB
    char public_mess[CMD_SIZE];
    snprintf(public_mess, CMD_SIZE, AT_CMD_PUB_MQTT, strlen(public_data));

    // Gán lại command mới trong mảng struct at_cmd
    memcpy(at_cmd[len - 3].command, public_mess, strlen(public_mess));
    memcpy(at_cmd[len - 2].command, public_data, strlen(public_data));
    // In ra thông báo khi xử lý xong
    ESP_LOGI("Set_CLBS_data", "SUCCESS");
}

void data_uart_handler(char *data, int len) {
    // In ra phản hồi khi nhận được
    ESP_LOGI("Response", "%s", data);
    // Sao chép dữ liệu sang recv_data để xử lý bên ngoài uart_task
    strcpy(recv_data, data);

    if (while_send) {
        // Chỉ thực hiện xử lý dữ liệu khi đang trong quá trình gửi
        if (strstr(data, at_cmd[cur_cmd_id].res_success) != NULL)
            // Nếu phản hồi thành công
            xEventGroupSetBits(xEventGroup, AT_CMD_SUCCESS_BIT);
        else {
            // Nếu phản hồi thất bại in ra cảnh báo và thiết lập cờ is_resend
            ESP_LOGW("Fail_response", "%s", data);
            is_resend = true;
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
    vTaskDelay(1500 / portTICK_PERIOD_MS);
    gpio_set_level(POWER_GPIO_PIN, 1);
    ESP_LOGI("STATUS", "Power on");
    vTaskDelay(5000 / portTICK_PERIOD_MS);
}

bool send_at_cmd(int cmd_id) {
    // Gán lại cur_cmd_id để tránh mất đồng bộ nếu không gửi lệnh theo thứ tự
    cur_cmd_id = cmd_id;
    // Đánh dấu đang trong quá trình gửi command
    while_send = true;
    // Lấy số lần gửi lại tối đa cho comamnd hiện tại
    int count = at_cmd[cmd_id].resend;

    // Gửi lệnh cho đến khi gửiạ lệnh thành công hoặc số lần gửi li bằng 0
    while (cur_cmd_id == cmd_id && count > 0) {
        if (is_resend) {
            // Cờ is_resend sẽ chỉ được thiết lập khi nhận được phản hồi không
            // thành không. Do đó ta sẽ thực hiện trễ 3s để tránh việc gửi lệnh
            // liên tục rồi chỉ nhận lại error response.
            vTaskDelay(3000 / portTICK_PERIOD_MS);
            ESP_LOGI("Resend", "%s", at_cmd[cmd_id].command);
        } else {
            ESP_LOGI("Send", "%s", at_cmd[cmd_id].command);
        }

        // Ghi command xuống UART
        uart_write_data(at_cmd[cmd_id].command, strlen(at_cmd[cmd_id].command));

        // Đợi phản hồi từ module cho đến khi timeout
        uxBits = xEventGroupWaitBits(
            xEventGroup, AT_CMD_SUCCESS_BIT | AT_CMD_FAILURE_BIT, pdTRUE,
            pdFALSE, pdMS_TO_TICKS(at_cmd[cmd_id].timeout));
        if (uxBits & AT_CMD_SUCCESS_BIT) {
            // Nếu phản hồi là thành công thực hiện xử lý dữ liệu và thực hiện
            // lệnh tiếp theo ngay sau đó 1s (1s trễ ở hàm main)
            if (strstr(at_cmd[cmd_id].command, "AT+CENG?") != NULL) {
                data_CENG_handler(recv_data);
            } else if (strstr(at_cmd[cmd_id].command, "AT+CLBS=1,0") != NULL) {
                data_CLBS_handler(recv_data);
            }
            cur_cmd_id++;
        } else {
            // Gửi thất bại hoặc timeout giảm số lần gửi lại đi 1
            count--;
        }
    }
    // Xóa các cờ cần thiết
    is_resend = false;
    while_send = false;

    // Trả về kết quả gửi lệnh
    if (count <= 0)
        return false;
    else
        return true;
}

void app_main(void) {
    // Thiết lập nguồn điện
    powerOnSetup();
    // Khởi tạo UART và thiết lập callback xử lý dữ liệu từ UART
    uart_init(EX_UART_NUM, GPIO_NUM_17, GPIO_NUM_16, 4096, data_uart_handler);
    // Event Group để lắng nghe sự kiện gửi lệnh thành công hoặc thất bại
    xEventGroup = xEventGroupCreate();
    // Lấy độ dài mảng struct at_cmd
    len = sizeof(at_cmd) / sizeof(at_cmd[0]);

    while (1) {
        // Thời gian bắt đầu bật KIT
        start = xTaskGetTickCount();

        powerOn();
        for (int id = 0; id < len; id++) {
            isSuccess = send_at_cmd(id);
            if (!isSuccess) {
                // Nếu 1 cmd bất kỳ bị gửi thất bại thực hiện tắt module
                ESP_LOGI("STATUS", "FAILURE");
                // Đưa id = len - 2 để lệnh tiếp theo được thực hiện là len - 1
                id = len - 2;
            }
            // Trễ 1s sau mỗi lần gửi để thực hiện xử lý dữ liệu nhận được (nếu
            // có)
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

        // Thời gian tắt KIT
        finish = xTaskGetTickCount();

        // Thực hiện trễ sao cho mỗi lần bật nguồn cách nhau 5 phút
        vTaskDelay((5 * 60000 / portTICK_PERIOD_MS) - (finish - start));
    }
}
