/* SDDC Smart Lock Demo

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp32_connect.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "sddc.h"
#include "cJSON.h"

#include "driver/gpio.h"
#include "camera.h"

static const char *TAG = "smart_lock";

#define GPIO_INPUT_IO_SMARTCOFNIG     12 

#define ESP_KEY_TASK_STACK_SIZE       4096
#define ESP_KEY_TASK_PRIO             25

#define ESP_SDDC_TASK_STACK_SIZE      4096
#define ESP_SDDC_TASK_PRIO            10

#define ESP_CONNECTOR_TASK_STACK_SIZE 4096
#define ESP_CONNECTOR_TASK_PRIO       20

static camera_pixelformat_t s_pixel_format;

static QueueHandle_t conn_mqueue_handle;

static TimerHandle_t lock_timer_handle;

#ifndef min
#define min(a, b)  ((a) < (b) ? (a) : (b))
#endif

#define CAMERA_PIXEL_FORMAT CAMERA_PF_JPEG
#define CAMERA_FRAME_SIZE   CAMERA_FS_SVGA

static struct timeval last_capture_time;

/*
 * Send image to connector
 */
static void esp_send_image(sddc_connector_t *conn)
{
    void *data;
    size_t size;
    size_t totol_len = 0;
    size_t len;
    int ret;
    struct timeval cur_time;
    struct timeval diff_time;
    long diff_msec;

    gettimeofday(&cur_time, NULL);

    timersub(&cur_time, &last_capture_time, &diff_time);
    diff_msec = (diff_time.tv_sec * 1000) + (diff_time.tv_usec / 1000);
    if (diff_msec >= 500) {
        camera_run();
        last_capture_time = cur_time;
    }

    data = camera_get_fb();
    size = camera_get_data_size();

    while (totol_len < size) {
        len = min((size - totol_len), (1460 - 16));

        ret = sddc_connector_put(conn, data, len, (totol_len + len) == size);
        if (ret < 0) {
            sddc_printf("Failed to put!\n");
            break;
        }
        totol_len += len;
        data += len;

        sddc_printf("Put %d byte\n", len);
    }

    sddc_printf("Total put %d byte\n", totol_len);
}

/*
 * sddc connector task
 */
static void esp_connector_task(void *arg)
{
    sddc_connector_t *conn;
    BaseType_t ret;

    while (1) {
        ret = xQueueReceive(conn_mqueue_handle, &conn, portMAX_DELAY);
        if (ret == pdTRUE) {
            esp_send_image(conn);
            sddc_connector_destroy(conn);
        }
    }

    vTaskDelete(NULL);
}

/*
 * Lock timer callback function
 */
static void esp_lock_timer_callback(TimerHandle_t handle)
{
    sddc_printf("Close the door!\n");
}

/*
 * Handle MESSAGE
 */
static sddc_bool_t esp_on_message(sddc_t *sddc, const uint8_t *uid, const char *message, size_t len)
{
    cJSON *root = cJSON_Parse(message);
    cJSON *cmd;
    char *str;

    sddc_return_value_if_fail(root, SDDC_TRUE);

    str = cJSON_Print(root);
    sddc_goto_error_if_fail(str);

    sddc_printf("esp_on_message: %s\n", str);
    cJSON_free(str);

    cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd)) {
        if (strcmp(cmd->valuestring, "recv") == 0) {

        } else if (strcmp(cmd->valuestring, "unlock") == 0) {
            cJSON *timeout = cJSON_GetObjectItem(root, "timeout");
            uint32_t timeout_ms;

            if (timeout && cJSON_IsNumber(timeout)) {
                timeout_ms = timeout->valuedouble;
                if (timeout_ms < 2000) {
                    timeout_ms = 2000;
                }
            } else {
                timeout_ms = 5000;
            }

            xTimerChangePeriod(lock_timer_handle, timeout_ms / portTICK_RATE_MS, 1);
            xTimerStart(lock_timer_handle, 0);

            sddc_printf("Open the door, timeout %dms!\n", timeout_ms);
            goto done;
        } else {
            sddc_printf("Command no support!\n");
            goto error;
        }

        cJSON *connector = cJSON_GetObjectItem(root, "connector");
        sddc_goto_error_if_fail(cJSON_IsObject(connector));

        cJSON *port = cJSON_GetObjectItem(connector, "port");
        sddc_goto_error_if_fail(cJSON_IsNumber(port));

        cJSON *token = cJSON_GetObjectItem(connector, "token");
        sddc_goto_error_if_fail(!token || cJSON_IsString(token));

        sddc_connector_t *conn = sddc_connector_create(sddc, uid, port->valuedouble, token ? token->valuestring : NULL, SDDC_FALSE);
        sddc_goto_error_if_fail(conn);

        int ret = xQueueSend(conn_mqueue_handle, &conn, 0);
        if (ret != pdTRUE) {
            sddc_connector_destroy(conn);
            sddc_goto_error_if_fail(ret == pdTRUE);
        }
    } else {
        sddc_printf("Command no specify!\n");
    }

done:
error:
    cJSON_Delete(root);

    return SDDC_TRUE;
}

/*
 * Handle MESSAGE ACK
 */
static void esp_on_message_ack(sddc_t *sddc, const uint8_t *uid, uint16_t seqno)
{
}

/*
 * Handle MESSAGE lost
 */
static void esp_on_message_lost(sddc_t *sddc, const uint8_t *uid, uint16_t seqno)
{
}

/*
 * Handle EdgerOS lost
 */
static void esp_on_edgeros_lost(sddc_t *sddc, const uint8_t *uid)
{
}

/*
 * Handle UPDATE
 */
static sddc_bool_t esp_on_update(sddc_t *sddc, const uint8_t *uid, const char *udpate_data, size_t len)
{
    cJSON *root = cJSON_Parse(udpate_data);
    char *str;

    sddc_return_value_if_fail(root, SDDC_FALSE);

    /*
     * Parse here
     */

    str = cJSON_Print(root);
    sddc_goto_error_if_fail(str);

    sddc_printf("esp_on_update: %s\n", str);
    cJSON_free(str);

    cJSON_Delete(root);

    return SDDC_TRUE;

error:
    cJSON_Delete(root);

    return SDDC_FALSE;
}

/*
 * Handle INVITE
 */
static sddc_bool_t esp_on_invite(sddc_t *sddc, const uint8_t *uid, const char *invite_data, size_t len)
{
    cJSON *root = cJSON_Parse(invite_data);
    char *str;

    sddc_return_value_if_fail(root, SDDC_FALSE);

    /*
     * Parse here
     */

    str = cJSON_Print(root);
    sddc_goto_error_if_fail(str);

    sddc_printf("esp_on_invite: %s\n", str);
    cJSON_free(str);

    cJSON_Delete(root);

    return SDDC_TRUE;

error:
    cJSON_Delete(root);

    return SDDC_FALSE;
}

/*
 * Handle the end of INVITE
 */
static sddc_bool_t esp_on_invite_end(sddc_t *sddc, const uint8_t *uid)
{
    return SDDC_TRUE;
}

/*
 * Create REPORT data
 */
static char *esp_report_data_create(void)
{
    cJSON *root;
    cJSON *report;
    char *str;

    root = cJSON_CreateObject();
    sddc_return_value_if_fail(root, NULL);

    report = cJSON_CreateObject();
    sddc_return_value_if_fail(report, NULL);

    cJSON_AddItemToObject(root, "report", report);
    cJSON_AddStringToObject(report, "name",   "IoT Camera");
    cJSON_AddStringToObject(report, "type",   "device");
    cJSON_AddBoolToObject(report,   "excl",   SDDC_FALSE);
    cJSON_AddStringToObject(report, "desc",   "翼辉 IoT Camera");
    cJSON_AddStringToObject(report, "model",  "1");
    cJSON_AddStringToObject(report, "vendor", "ACOINFO");
    cJSON_AddStringToObject(report, "sn",     "123456");

    /*
     * Add extension here
     */

    str = cJSON_Print(root);
    sddc_return_value_if_fail(str, NULL);

    sddc_printf("REPORT DATA: %s\n", str);

    cJSON_Delete(root);

    return str;
}

/*
 * Create INVITE data
 */
static char *esp_invite_data_create(void)
{
    cJSON *root;
    cJSON *report;
    char *str;

    root = cJSON_CreateObject();
    sddc_return_value_if_fail(root, NULL);

    report = cJSON_CreateObject();
    sddc_return_value_if_fail(report, NULL);

    cJSON_AddItemToObject(root, "report", report);
    cJSON_AddStringToObject(report, "name",   "IoT Camera");
    cJSON_AddStringToObject(report, "type",   "device");
    cJSON_AddBoolToObject(report,   "excl",   SDDC_FALSE);
    cJSON_AddStringToObject(report, "desc",   "翼辉 IoT Camera");
    cJSON_AddStringToObject(report, "model",  "1");
    cJSON_AddStringToObject(report, "vendor", "ACOINFO");

    /*
     * Add extension here
     */
    /*
     * See sddc.h Update and Invite Data example
     *
    cJSON *server = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "server", server);

    cJSON *vsoa = cJSON_CreateArray();
    cJSON_AddItemToObject(server, "vsoa", vsoa);

    cJSON *vsoa_xxx_server = cJSON_CreateObject();
    cJSON_AddStringToObject(vsoa_xxx_server, "desc", "vsoa xxx server");
    cJSON_AddNumberToObject(vsoa_xxx_server, "port",  1234);

    cJSON_AddItemToArray(vsoa, vsoa_xxx_server);
    */

    str = cJSON_Print(root);
    sddc_return_value_if_fail(str, NULL);

    sddc_printf("INVITE DATA: %s\n", str);

    cJSON_Delete(root);

    return str;
}

/*
 * key task
 */
static void esp_key_task(void *arg)
{
    sddc_t *sddc = arg;
    int i = 0;

    while (1) {
        vTaskDelay(50 / portTICK_RATE_MS);

        if (!gpio_get_level(GPIO_INPUT_IO_SMARTCOFNIG)) {
            i++;
            if (i > (3 * 20)) {
                i = 0;
                sddc_printf("Start SmartConfig....\n");
                example_smart_config();
            }
        } else {
            if (i > 0) {
                cJSON *root = NULL;
                char *str;
                size_t size;
                int ret;

                ret = camera_run();
                sddc_goto_error_if_fail(ret == ESP_OK);

                gettimeofday(&last_capture_time, NULL);
                
                size = camera_get_data_size();

                root = cJSON_CreateObject();
                sddc_goto_error_if_fail(root);

                cJSON_AddStringToObject(root, "cmd", "recv");
                cJSON_AddNumberToObject(root, "size", size);

                sddc_printf("Send picture to EdgerOS, file size %d\n", size);

                str = cJSON_Print(root);
                sddc_goto_error_if_fail(str);

                sddc_broadcast_message(sddc, str, strlen(str), 1, SDDC_FALSE, NULL);
                cJSON_free(str);

error:
                cJSON_Delete(root);
            }
            i = 0;
        }
    }

    vTaskDelete(NULL);
}

/*
 * sddc protocol task
 */
static void esp_sddc_task(void *arg)
{
    sddc_t *sddc = arg;
    char *data;
    uint8_t mac[6];
    char ip[sizeof("255.255.255.255")];
    tcpip_adapter_ip_info_t ip_info = { 0 };

    /*
     * Set call backs
     */
    sddc_set_on_message(sddc, esp_on_message);
    sddc_set_on_message_ack(sddc, esp_on_message_ack);
    sddc_set_on_message_lost(sddc, esp_on_message_lost);
    sddc_set_on_invite(sddc, esp_on_invite);
    sddc_set_on_invite_end(sddc, esp_on_invite_end);
    sddc_set_on_update(sddc, esp_on_update);
    sddc_set_on_edgeros_lost(sddc, esp_on_edgeros_lost);

    /*
     * Set token
     */
#if SDDC_CFG_SECURITY_EN > 0
    sddc_set_token(sddc, "1234567890");
#endif

    /*
     * Set report data
     */
    data = esp_report_data_create();
    sddc_set_report_data(sddc, data, strlen(data));

    /*
     * Set invite data
     */
    data = esp_invite_data_create();
    sddc_set_invite_data(sddc, data, strlen(data));

    /*
     * Get mac address
     */
    esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);

    /*
     * Set uid
     */
    sddc_set_uid(sddc, mac);

    /*
     * Get and print ip address
     */
    tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info);

    inet_ntoa_r(ip_info.ip, ip, sizeof(ip));

    sddc_printf("IP addr: %s\n", ip);

    /*
     * SDDC run
     */
    while (1) {
        sddc_printf("SDDC running...\n");

        sddc_run(sddc);

        sddc_printf("SDDC quit!\n");
    }

    /*
     * Destroy SDDC
     */
    sddc_destroy(sddc);

    vTaskDelete(NULL);
}

/*
 * Init camera
 */
static esp_err_t esp_cam_init(void)
{
   camera_config_t camera_config = {
        .ledc_channel = LEDC_CHANNEL_0,
        .ledc_timer = LEDC_TIMER_0,
        .pin_d0 = CONFIG_D0,
        .pin_d1 = CONFIG_D1,
        .pin_d2 = CONFIG_D2,
        .pin_d3 = CONFIG_D3,
        .pin_d4 = CONFIG_D4,
        .pin_d5 = CONFIG_D5,
        .pin_d6 = CONFIG_D6,
        .pin_d7 = CONFIG_D7,
        .pin_xclk = CONFIG_XCLK,
        .pin_pclk = CONFIG_PCLK,
        .pin_vsync = CONFIG_VSYNC,
        .pin_href = CONFIG_HREF,
        .pin_sscb_sda = CONFIG_SDA,
        .pin_sscb_scl = CONFIG_SCL,
        .pin_reset = CONFIG_RESET,
        .xclk_freq_hz = CONFIG_XCLK_FREQ,
    };

    camera_model_t camera_model;
    esp_err_t err;
    uint32_t start_code;

__init_camera:
    err = camera_probe(&camera_config, &camera_model);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera probe failed with error 0x%x", err);
        return ESP_FAIL;
    }

    if (camera_model == CAMERA_OV7725) {
        s_pixel_format = CAMERA_PIXEL_FORMAT;
        camera_config.frame_size = CAMERA_FRAME_SIZE;
        ESP_LOGI(TAG, "Detected OV7725 camera, using %s bitmap format",
                CAMERA_PIXEL_FORMAT == CAMERA_PF_GRAYSCALE ?
                        "grayscale" : "RGB565");
    } else if (camera_model == CAMERA_OV2640) {
        ESP_LOGI(TAG, "Detected OV2640 camera, using JPEG format");
        s_pixel_format = CAMERA_PIXEL_FORMAT;
        camera_config.frame_size = CAMERA_FRAME_SIZE;
        if (s_pixel_format == CAMERA_PF_JPEG)
            camera_config.jpeg_quality = 15;
    } else {
        ESP_LOGE(TAG, "Camera not supported");
        return ESP_FAIL;
    }

    camera_config.pixel_format = s_pixel_format;
    err = camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return ESP_FAIL;
    }

    err = camera_run();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera run failed with error 0x%x", err);
        camera_deinit();
        goto __init_camera;
    }

    start_code = *(uint32_t *)camera_get_fb();
    if (start_code != 0xe0ffd8ff) {
        ESP_LOGE(TAG, "Camera data start code error 0x%x", start_code);
        camera_deinit();
        goto __init_camera;
    }

    gettimeofday(&last_capture_time, NULL);

    return ESP_OK;
}

/*
 * Init gpio
 */
static esp_err_t esp_gpio_init(void)
{
    gpio_config_t io_conf;

    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL << GPIO_INPUT_IO_SMARTCOFNIG;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    ESP_ERROR_CHECK(esp_gpio_init());
    ESP_ERROR_CHECK(esp_cam_init());

    conn_mqueue_handle = xQueueCreate(4, sizeof(sddc_connector_t *));

    lock_timer_handle  = xTimerCreate("lock_timer",
                                      2000 / portTICK_RATE_MS,
                                      pdFALSE,
                                      0,
                                      esp_lock_timer_callback);
    /*
     * Create SDDC
     */
    sddc_t *sddc = sddc_create(SDDC_CFG_PORT);

    xTaskCreate(esp_sddc_task, "sddc_task", ESP_SDDC_TASK_STACK_SIZE, sddc, ESP_SDDC_TASK_PRIO, NULL);
    xTaskCreate(esp_key_task, "key_task", ESP_KEY_TASK_STACK_SIZE, sddc, ESP_KEY_TASK_PRIO, NULL);
    xTaskCreate(esp_connector_task, "connector_task1", ESP_CONNECTOR_TASK_STACK_SIZE, sddc, ESP_CONNECTOR_TASK_PRIO, NULL);
}
