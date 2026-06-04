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
#include "uart_modbus.h"   // ← scan 함수 참조
#include <stdarg.h>

#define TAG "WIFI_MQTT"

static QueueHandle_t            s_log_queue     = NULL;
static bool                     s_mqtt_connected = false;
static bool                     s_log_enabled    = true;
static esp_mqtt_client_handle_t s_mqtt_client    = NULL;
static EventGroupHandle_t       s_eth_event_group;
static EventGroupHandle_t       s_net_events;

#define ETH_GOT_IP_BIT  BIT0
#define IP_ACQUIRED_BIT BIT0

// ════════════════════════════════════════════════════════════
// ── Modbus 스캔 (mqtt_event_handler보다 위에 선언)
// ════════════════════════════════════════════════════════════

typedef struct {
    uint8_t  start;
    uint8_t  end;
    uint16_t timeout_ms;
} scan_params_t;

// 각 ID 결과마다 호출되는 콜백
// → found=true  : modbus/scan/result  publish (QoS 1)
// → 매 ID마다   : modbus/scan/progress publish
static void scan_result_cb(uint8_t id, bool found, void *user_data)
{
    if (s_mqtt_client == NULL) return;

    scan_params_t *p = (scan_params_t *)user_data;

    // ── progress publish ──────────────────────────────
    char prog[64];
    snprintf(prog, sizeof(prog),
             "{\"current\":%d,\"total\":%d}", id, p->end);
    esp_mqtt_client_publish(s_mqtt_client,
                            "modbus/scan/progress", prog, 0, 0, 0);

    // ── found 시 result publish ───────────────────────
    if (found) {
        char result[64];
        snprintf(result, sizeof(result),
                 "{\"id\":%d,\"status\":\"found\"}", id);
        esp_mqtt_client_publish(s_mqtt_client,
                                "modbus/scan/result", result, 0, 1, 0);
        ESP_LOGI(TAG, "스캔 결과 publish: ID=%d", id);
    }
}

// scan_task: MQTT_EVENT_DATA 에서 xTaskCreate로 생성
// payload 형식: {"start":1,"end":20,"timeout_ms":200}
static void scan_task(void *arg)
{
    scan_params_t *p = (scan_params_t *)arg;

    ESP_LOGI(TAG, "scan_task 시작: %d~%d timeout=%dms",
             p->start, p->end, p->timeout_ms);

    // 스캔 시작 알림
    if (s_mqtt_client) {
        char status[64];
        snprintf(status, sizeof(status),
                 "{\"status\":\"scanning\",\"start\":%d,\"end\":%d}",
                 p->start, p->end);
        esp_mqtt_client_publish(s_mqtt_client,
                                "modbus/scan/status", status, 0, 0, 0);
    }

    // 실제 스캔 수행 (콜백으로 결과 실시간 publish)
    modbus_scan_slaves_with_cb(p->start, p->end,
                                p->timeout_ms,
                                scan_result_cb, p);

    // 완료 publish
    if (s_mqtt_client) {
        esp_mqtt_client_publish(s_mqtt_client,
                                "modbus/scan/done",
                                "{\"status\":\"done\"}", 0, 0, 0);
        ESP_LOGI(TAG, "scan_task 완료");
    }

    free(p);
    vTaskDelete(NULL);
}

// ════════════════════════════════════════════════════════════
// ── 로그 전송
// ════════════════════════════════════════════════════════════

static int mqtt_log_vprintf(const char *fmt, va_list args)
{
    va_list args2;
    va_copy(args2, args);
    int ret = vprintf(fmt, args);

    if (s_mqtt_connected && s_log_queue != NULL) {
        char *buf = malloc(128);
        if (buf) {
            vsnprintf(buf, 128, fmt, args2);
            if (xQueueSend(s_log_queue, &buf, 0) != pdTRUE)
                free(buf);
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
            if (s_mqtt_connected && s_mqtt_client != NULL && s_log_enabled)
                esp_mqtt_client_publish(s_mqtt_client, "esp32/log", buf, 0, 0, 0);
            free(buf);
        }
    }
}

// ════════════════════════════════════════════════════════════
// ── ENC28J60 IP 이벤트 핸들러
// ════════════════════════════════════════════════════════════

static void eth_got_ip_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    const app_config_t *cfg = (const app_config_t *)arg;
    ip_event_got_ip_t  *event = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "ENC28J60 IP 할당: " IPSTR " / GW: " IPSTR,
             IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.gw));
    xEventGroupSetBits(s_eth_event_group, ETH_GOT_IP_BIT);
    xEventGroupSetBits(s_net_events, IP_ACQUIRED_BIT);
    mqtt_start(cfg);
}

static void eth_lost_ip_handler(void *arg, esp_event_base_t base,
                                 int32_t id, void *data)
{
    ESP_LOGW(TAG, "IP 해제됨 → DHCP 재시도");
    xEventGroupClearBits(s_eth_event_group, ETH_GOT_IP_BIT);
    esp_netif_t *netif = enc28j60_get_netif();
    esp_netif_dhcpc_stop(netif);
    esp_netif_dhcpc_start(netif);
}

// ════════════════════════════════════════════════════════════
// ── MQTT 이벤트 핸들러
// ════════════════════════════════════════════════════════════

static void mqtt_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t  event  = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch (event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT 브로커 연결 완료");
        s_mqtt_connected = true;
        esp_mqtt_client_subscribe(client, "ota/update",          1);
        esp_mqtt_client_subscribe(client, "log/control",         1);
        esp_mqtt_client_subscribe(client, "config/enter",        1);
        esp_mqtt_client_subscribe(client, "device/restart",      1);
        esp_mqtt_client_subscribe(client, "modbus/scan/request", 1); // ← 추가
        // IP publish
        {
            esp_netif_ip_info_t ip_info;
            esp_netif_t *netif = enc28j60_get_netif();
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                char ip_str[32];
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                esp_mqtt_client_publish(s_mqtt_client, "esp32/info",
                                        ip_str, 0, 0, 0);
            }
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT 연결 끊김 → 자동 재연결");
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT 에러");
        break;

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
        else if (strncmp(event->topic, "modbus/scan/request",
                         event->topic_len) == 0) {
            // payload: {"start":1,"end":20,"timeout_ms":200}
            scan_params_t *p = malloc(sizeof(scan_params_t));
            if (p == NULL) break;

            // 기본값
            p->start      = 1;
            p->end        = 20;
            p->timeout_ms = 200;

            // 파싱 (cJSON 의존성 없이 sscanf)
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "%.*s",
                     event->data_len, event->data);

            int s = 0, e = 0, t = 0;
            if (sscanf(tmp,
                       "{\"start\":%d,\"end\":%d,\"timeout_ms\":%d}",
                       &s, &e, &t) >= 2) {
                if (s >= 1 && s <= 247) p->start      = (uint8_t)s;
                if (e >= 1 && e <= 247) p->end        = (uint8_t)e;
                if (t >= 50 && t <= 2000) p->timeout_ms = (uint16_t)t;
            }

            ESP_LOGI(TAG, "스캔 요청: %d~%d timeout=%dms",
                     p->start, p->end, p->timeout_ms);

            xTaskCreate(scan_task, "scan_task", 4096, p, 4, NULL);
        }
        break;

    default:
        break;
    }
}

// ════════════════════════════════════════════════════════════
// ── 공개 API
// ════════════════════════════════════════════════════════════

esp_err_t wifi_mqtt_init_with_config(const app_config_t *cfg)
{
    s_net_events      = xEventGroupCreate();
    s_eth_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP,
        &eth_got_ip_handler, (void *)cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_ETH_LOST_IP,
        &eth_lost_ip_handler, NULL));

    return ESP_OK;
}

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
            .reconnect_timeout_ms = 5000,
        },
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) return ESP_FAIL;

    s_mqtt_client = client;
    ota_set_client(client);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    s_log_queue = xQueueCreate(20, sizeof(char *));
    xTaskCreate(log_publish_task, "log_pub", 4096, NULL, 3, NULL);
    esp_log_set_vprintf(mqtt_log_vprintf);

    return ESP_OK;
}

void mqtt_publish_sensor(float temperature, float humidity)
{
    if (s_mqtt_client == NULL) return;

    char payload[64];
    snprintf(payload, sizeof(payload),
             "{\"temp\":%.1f,\"humi\":%.1f,\"id\":\"device_01\"}",
             temperature, humidity);
    esp_mqtt_client_publish(s_mqtt_client, "sensor/device_01/data",
                            payload, 0, 0, 0);
    ESP_LOGI(TAG, "Publish → 온도:%.1f℃  습도:%.1f%%",
             temperature, humidity);
}