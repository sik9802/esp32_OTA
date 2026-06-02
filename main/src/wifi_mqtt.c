#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "wifi_mqtt.h"
#include "enc28j60.h"
#include "ota.h"
#include <stdarg.h>




#define TAG          "WIFI_MQTT"

/* cloud Define 관련
// ── HiveMQ Cloud 브로커 설정 ───────────────────
#define MQTT_BROKER_URI  "mqtts://604efa86dab4428c9f2f4de139f5ca0a.s1.eu.hivemq.cloud:8883"
#define MQTT_USERNAME    "chunsik"
#define MQTT_PASSWORD    "ESP32server"
*/

static QueueHandle_t s_log_queue = NULL;
static bool s_mqtt_connected = false;
static bool s_log_enabled = true;

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static EventGroupHandle_t s_eth_event_group;
#define ETH_GOT_IP_BIT BIT0
static EventGroupHandle_t s_net_events;
#define IP_ACQUIRED_BIT  BIT0


// ── ENC28J60 IP 할당 이벤트 ────────────────────
static void eth_got_ip_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    const app_config_t *cfg = (const app_config_t *)arg;  // ← 추가
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "ENC28J60 IP 할당: " IPSTR " / GW: " IPSTR,
             IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.gw));
    xEventGroupSetBits(s_eth_event_group, ETH_GOT_IP_BIT);
    xEventGroupSetBits(s_net_events, IP_ACQUIRED_BIT);  // ← 추가
    // ← 여기서 MQTT 시작 (IP 확보 보장됨)
    mqtt_start(cfg);
}
static void eth_lost_ip_handler(void *arg, esp_event_base_t base,
                                 int32_t id, void *data)
{
    ESP_LOGW(TAG, "IP 해제됨 → DHCP 재시도");
    xEventGroupClearBits(s_eth_event_group, ETH_GOT_IP_BIT);
    esp_netif_t *netif = enc28j60_get_netif();   // ← 이렇게 변경
    esp_netif_dhcpc_stop(netif);
    esp_netif_dhcpc_start(netif);
}

static int mqtt_log_vprintf(const char *fmt, va_list args)
{
    va_list args2;
    va_copy(args2, args);
    int ret = vprintf(fmt, args);

    if (s_mqtt_connected && s_log_queue != NULL) {
        char *buf = malloc(128);
        if (buf) {
            vsnprintf(buf, 128, fmt, args2);
            if (xQueueSend(s_log_queue, &buf, 0) != pdTRUE) {
                free(buf);
            }
        }
    }

    va_end(args2);
    return ret;
}


static void log_publish_task(void *arg)
{
    char *buf;
    while (1) {
        if (xQueueReceive(s_log_queue, &buf, portMAX_DELAY) == pdTRUE) {
            if (s_mqtt_connected && s_mqtt_client != NULL && s_log_enabled) {  // ← s_log_enabled 추가
                esp_mqtt_client_publish(
                    s_mqtt_client, "esp32/log", buf, 0, 0, 0);
            }
            free(buf);
        }
    }
}

// ── MQTT 이벤트 핸들러 ─────────────────────────
static void mqtt_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;  // ← 이 줄 추가
    esp_mqtt_client_handle_t client = event->client;  // ← 이 줄 추가

    switch (event_id) {
        
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT 브로커 연결 완료");
        s_mqtt_connected = true;                           // ← 추가
        esp_mqtt_client_subscribe(client, "ota/update", 1);
        esp_mqtt_client_subscribe(client, "log/control", 1);  // ← 추가
        esp_mqtt_client_subscribe(client, "config/enter",   1);  // ← 추가
        esp_mqtt_client_subscribe(client, "device/restart", 1);  // ← 추가
        // IP publish
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = enc28j60_get_netif();
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char ip_str[32];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            esp_mqtt_client_publish(s_mqtt_client, "esp32/info", ip_str, 0, 0, 0);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;                          // ← 추가
        ESP_LOGW(TAG, "MQTT 연결 끊김 → 자동 재연결");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT 에러");
        break;
    // mqtt 토픽 데이터
    case MQTT_EVENT_DATA:
        if (strncmp(event->topic, "ota/update", event->topic_len) == 0) {
            static char url[256];
            snprintf(url, sizeof(url), "%.*s", event->data_len, event->data);
            ESP_LOGI(TAG, "OTA 트리거: %s", url);
            xTaskCreate(ota_task, "ota_task", 8192, url, 5, NULL);
        }
        else if (strncmp(event->topic, "log/control", event->topic_len) == 0) {
            if (strncmp(event->data, "stop", event->data_len) == 0) {
                s_log_enabled = false;
                ESP_LOGI(TAG, "로그 전송 중단");
            } else if (strncmp(event->data, "start", event->data_len) == 0) {
                s_log_enabled = true;
                ESP_LOGI(TAG, "로그 전송 재개");
            }
        }
        else if (strncmp(event->topic, "config/enter", event->topic_len) == 0) {
                    xTaskCreate((TaskFunction_t)config_portal_request,
                                "portal_task", 4096, NULL, 5, NULL);
        }
        else if (strncmp(event->topic, "device/restart", event->topic_len) == 0) {
            ESP_LOGW(TAG, "원격 재시작 명령 수신");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
        break;
    
    default:
        break;
}
}

// ── 공개 API ───────────────────────────────────
esp_err_t wifi_mqtt_init_with_config(const app_config_t *cfg)
{
    s_net_events = xEventGroupCreate();
    /*// NVS 초기화 (MQTT 내부에서 필요)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);  
    */

    //ESP_ERROR_CHECK(esp_event_loop_create_default());

        // ENC28J60 IP 할당 대기용 이벤트 그룹
    s_eth_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP,
        &eth_got_ip_handler, (void *)cfg));  // ← NULL → cfg

    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_ETH_LOST_IP,
        &eth_lost_ip_handler, NULL));

    return ESP_OK;
}

// 대기 함수 추가:
void wait_for_ip(void)
{
    xEventGroupWaitBits(s_net_events, IP_ACQUIRED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

esp_err_t mqtt_start(const app_config_t *cfg)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = cfg->mqtt_broker,
            .verification.crt_bundle_attach = esp_crt_bundle_attach,
        },
        .credentials = {
            .username = cfg->mqtt_user,
            .authentication.password = cfg->mqtt_pass,
        },
        .network = {
        .reconnect_timeout_ms = 5000,  // ← 추가: 5초마다 재시도
        },
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }
    s_mqtt_client = client;
    ota_set_client(client);  // ← 추가
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
    s_log_queue = xQueueCreate(20, sizeof(char *));
    xTaskCreate(log_publish_task, "log_pub", 4096, NULL, 3, NULL);
    esp_log_set_vprintf(mqtt_log_vprintf);

    return ESP_OK;
}



/*  이전 mqtt_start(void)
esp_err_t mqtt_start(void)
{
    // IP 할당 대기 (최대 30초)
    ESP_LOGI(TAG, "ENC28J60 IP 할당 대기 중...");
    EventBits_t bits = xEventGroupWaitBits(
        s_eth_event_group, ETH_GOT_IP_BIT,
        pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));

    if (!(bits & ETH_GOT_IP_BIT)) {
        ESP_LOGE(TAG, "IP 할당 타임아웃");
        return ESP_FAIL;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri      = MQTT_BROKER_URI,
        .credentials.username    = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);

    vTaskDelay(pdMS_TO_TICKS(2000));
    return ESP_OK;
}
*/

void mqtt_publish_sensor(float temperature, float humidity)
{
    if (s_mqtt_client == NULL) return;

    char payload[64];
    snprintf(payload, sizeof(payload),
             "{\"temp\":%.1f,\"humi\":%.1f,\"id\":\"device_01\"}",
             temperature, humidity);
    esp_mqtt_client_publish(s_mqtt_client, "sensor/device_01/data",
                            payload, 0, 0, 0);
    ESP_LOGI(TAG, "Publish → 온도:%.1f℃  습도:%.1f%%", temperature, humidity);
}
