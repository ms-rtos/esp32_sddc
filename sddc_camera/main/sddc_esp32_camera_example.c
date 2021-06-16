/* SDDC Camera Example

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
#include "esp_wpa2.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "protocol_examples_common.h"
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

static EventGroupHandle_t wifi_event_group;
static const int CONNECTED_BIT = BIT0;
static const int ESPTOUCH_DONE_BIT = BIT1;
static const char *TAG = "sddc";

#define GPIO_INPUT_IO_SMARTCOFNIG     12 

#define ESP_KEY_TASK_STACK_SIZE       4096
#define ESP_KEY_TASK_PRIO             25

#define ESP_SDDC_TASK_STACK_SIZE      4096
#define ESP_SDDC_TASK_PRIO            10

#define ESP_CONNECTOR_TASK_STACK_SIZE 4096
#define ESP_CONNECTOR_TASK_PRIO       20

static camera_pixelformat_t s_pixel_format;

static QueueHandle_t conn_mqueue_handle = NULL;

#ifndef min
#define min(a, b)  ((a) < (b) ? (a) : (b))
#endif

#define CAMERA_PIXEL_FORMAT CAMERA_PF_JPEG
#define CAMERA_FRAME_SIZE   CAMERA_FS_SVGA

static struct timeval last_capture_time;

/*
 * Get picture from connector
 */
static void esp_get_pic(sddc_connector_t *conn)
{
    sddc_bool_t finish;
    void *data;
    ssize_t ret;
    size_t totol_len = 0;

    while (1) {
        ret = sddc_connector_get(conn, &data, &finish);
        if (ret < 0) {
            sddc_printf("Failed to get!\n");
            break;
        } else {
            sddc_printf("Get %d byte\n", ret);
            totol_len += ret;
            if (finish) {
                break;
            }
        }
    }

    sddc_printf("Total get %d byte\n", totol_len);

    sddc_connector_destroy(conn);
}

/*
 * Put picture to connector
 */
static void esp_put_pic(sddc_connector_t *conn)
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
        len = min((size - totol_len), 1024);

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
    sddc_connector_destroy(conn);
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
            if (sddc_connector_mode(conn) == 1) {
                esp_get_pic(conn);
            } else {
                esp_put_pic(conn);
            }
        }
    }
}

/*
 * handle MESSAGE
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

    /*
     * Parse here
     */
    cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd)) {
        sddc_bool_t get_mode;
        int ret;

        if (strcmp(cmd->valuestring, "recv") == 0) {
            get_mode = SDDC_FALSE;
        } else if (strcmp(cmd->valuestring, "send") == 0) {
            get_mode = SDDC_TRUE;

            cJSON *size = cJSON_GetObjectItem(root, "size");
            if (size && cJSON_IsNumber(size)) {
                sddc_printf("EdgerOS send picture to me, file size %d\n", (int)size->valuedouble);
            }
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

        sddc_connector_t *conn = sddc_connector_create(sddc, uid, port->valuedouble, token ? token->valuestring : NULL, get_mode);
        sddc_goto_error_if_fail(conn);

        ret = xQueueSend(conn_mqueue_handle, &conn, 0);
        if (ret != pdTRUE) {
            sddc_connector_destroy(conn);
            sddc_goto_error_if_fail(ret == pdTRUE);
        }

    } else {
        sddc_printf("Command no specify!\n");
    }

error:
    cJSON_Delete(root);

    return SDDC_TRUE;
}

/*
 * handle MESSAGE ACK
 */
static void esp_on_message_ack(sddc_t *sddc, const uint8_t *uid, uint16_t seqno)
{
}

/*
 * handle MESSAGE lost
 */
static void esp_on_message_lost(sddc_t *sddc, const uint8_t *uid, uint16_t seqno)
{
}

/*
 * handle EdgerOS lost
 */
static void esp_on_edgeros_lost(sddc_t *sddc, const uint8_t *uid)
{
}

/*
 * handle UPDATE
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
 * handle INVITE
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
 * handle the end of INVITE
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

    str = cJSON_Print(root);
    sddc_return_value_if_fail(str, NULL);

    sddc_printf("INVITE DATA: %s\n", str);

    cJSON_Delete(root);

    return str;
}

static void event_handle(void* arg, esp_event_base_t event_base, 
                         int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "Scan done");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "Found channel");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        ESP_LOGI(TAG, "Got SSID and password");

        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
        wifi_config_t wifi_config;
        uint8_t ssid[33] = { 0 };
        uint8_t password[65] = { 0 };

        bzero(&wifi_config, sizeof(wifi_config_t));
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
        wifi_config.sta.bssid_set = evt->bssid_set;
        if (wifi_config.sta.bssid_set == true) {
            memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
        }

        memcpy(ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(password, evt->password, sizeof(evt->password));
        ESP_LOGI(TAG, "SSID:%s", ssid);
        ESP_LOGI(TAG, "PASSWORD:%s", password);

        ESP_ERROR_CHECK( esp_wifi_disconnect() );
        ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
        esp_wifi_connect();
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
        xEventGroupSetBits(wifi_event_group, ESPTOUCH_DONE_BIT);
    }
}

/*
 * flash key task
 */
static void esp_flash_key_task(void *arg)
{
    sddc_t *sddc = arg;
    gpio_config_t io_conf;
    int i = 0;
    
    (void)sddc;

    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL << GPIO_INPUT_IO_SMARTCOFNIG;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    while (1) {
        vTaskDelay(50 / portTICK_RATE_MS);

        if (!gpio_get_level(GPIO_INPUT_IO_SMARTCOFNIG)) {
            i++;
            if (i > (3 * 20)) {
                i = 0;

                sddc_printf("Start SmartConfig....\n");

                esp_wifi_disconnect();
                xEventGroupClearBits(wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT);

                ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handle, NULL) );
                ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handle, NULL) );
                ESP_ERROR_CHECK( esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handle, NULL) );

                ESP_ERROR_CHECK( esp_smartconfig_set_type(SC_TYPE_ESPTOUCH) );

                smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
                ESP_ERROR_CHECK( esp_smartconfig_start(&cfg) );

                while (1) {
                    EventBits_t uxBits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT, true, false, portMAX_DELAY); 
                    if (uxBits & CONNECTED_BIT) {
                        ESP_LOGI(TAG, "Wi-Fi Connected to AP");
                    }
                    if (uxBits & ESPTOUCH_DONE_BIT) {
                        ESP_LOGI(TAG, "SmartConfig over");
                        esp_smartconfig_stop();
                        break;
                    }
                }

                ESP_ERROR_CHECK( esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handle) );
                ESP_ERROR_CHECK( esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handle) );
                ESP_ERROR_CHECK( esp_event_handler_unregister(SC_EVENT, ESP_EVENT_ANY_ID, &event_handle) );
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
}

/*
 * sddc protocol task
 */
static void esp_sddc_task(void *arg)
{
    sddc_t *sddc;
    char *data;
    uint8_t mac[6];
    char ip[sizeof("255.255.255.255")];
    tcpip_adapter_ip_info_t ip_info = { 0 };

    /*
     * Create SDDC
     */
    sddc = sddc_create(SDDC_CFG_PORT);

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

    wifi_event_group = xEventGroupCreate();

    xTaskCreate(esp_flash_key_task, "flash_key_task",  ESP_KEY_TASK_STACK_SIZE, sddc, ESP_KEY_TASK_PRIO, NULL);

    conn_mqueue_handle = xQueueCreate(4, sizeof(sddc_connector_t *));
    
    xTaskCreate(esp_connector_task, "connector_task1", ESP_CONNECTOR_TASK_STACK_SIZE, sddc, ESP_CONNECTOR_TASK_PRIO, NULL);
    
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
        return;
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
        return;
    }

    camera_config.pixel_format = s_pixel_format;
    err = camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return;
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

    xTaskCreate(esp_sddc_task, "sddc_task", ESP_SDDC_TASK_STACK_SIZE, NULL, ESP_SDDC_TASK_PRIO, NULL);
}
