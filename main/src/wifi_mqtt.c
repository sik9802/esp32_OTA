#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "wifi_mqtt.h"
#include "enc28j60.h"


#define TAG          "WIFI_MQTT"

// ── HiveMQ Cloud 브로커 설정 ───────────────────
#define MQTT_BROKER_URI  "mqtts://604efa86dab4428c9f2f4de139f5ca0a.s1.eu.hivemq.cloud:8883"
#define MQTT_USERNAME    "chunsik"
#define MQTT_PASSWORD    "ESP32server"

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static EventGroupHandle_t s_eth_event_group;
#define ETH_GOT_IP_BIT BIT0
static EventGroupHandle_t s_net_events;
#define IP_ACQUIRED_BIT  BIT0

// ── ENC28J60 IP 할당 이벤트 ────────────────────
static void eth_got_ip_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "ENC28J60 IP 할당: " IPSTR " / GW: " IPSTR,
             IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.gw));
    xEventGroupSetBits(s_eth_event_group, ETH_GOT_IP_BIT);
    xEventGroupSetBits(s_net_events, IP_ACQUIRED_BIT);  // ← 추가
    // ← 여기서 MQTT 시작 (IP 확보 보장됨)
    mqtt_start();
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

// ── MQTT 이벤트 핸들러 ─────────────────────────
static void mqtt_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT 브로커 연결 완료");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT 연결 끊김 → 자동 재연결");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT 에러");
            break;
        default:
            break;
    }
}

// ── 공개 API ───────────────────────────────────
esp_err_t wifi_mqtt_init(void)
{
    s_net_events = xEventGroupCreate();
    // NVS 초기화 (MQTT 내부에서 필요)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // ENC28J60 IP 할당 대기용 이벤트 그룹
    s_eth_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP,
        &eth_got_ip_handler, NULL));

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

esp_err_t mqtt_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = MQTT_BROKER_URI,
            .verification.crt_bundle_attach = esp_crt_bundle_attach,
        },
        .credentials = {
            .username = MQTT_USERNAME,
            .authentication.password = MQTT_PASSWORD,
        },
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }
    s_mqtt_client = client;
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

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
