#include "ota.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "OTA";
static esp_mqtt_client_handle_t s_client = NULL;

void ota_set_client(esp_mqtt_client_handle_t client)
{
    s_client = client;
}

static void publish_result(const char *status, const char *msg)
{
    if (s_client == NULL) return;
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"status\":\"%s\",\"msg\":\"%s\"}", status, msg);
    esp_mqtt_client_publish(s_client, "ota/result", payload, 0, 1, 0);
}

void ota_task(void *pvParameter)
{
    const char *url = (const char *)pvParameter;
    ESP_LOGI(TAG, "OTA 시작: %s", url);
    publish_result("start", "OTA 다운로드 시작");

    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size     = 2048,   // ← 추가
        .buffer_size_tx  = 2048,   // ← 추가
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    ESP_LOGI(TAG, "펌웨어 다운로드 중...");
    esp_err_t ret = esp_https_ota(&ota_cfg);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA 완료 → 3초 후 재시작");
        publish_result("ok", "OTA 완료. 재시작합니다.");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA 실패: %s", esp_err_to_name(ret));
        publish_result("fail", esp_err_to_name(ret));
    }

    vTaskDelete(NULL);  // ← 반드시 필요
}