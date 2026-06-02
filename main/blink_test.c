
#include "includes.h"

static const char *TAG = "MAIN";


 void app_main(void)
    {
        ESP_LOGI(TAG, "=== 앱 시작 ===");
 
        // WDT 설정 (기존 유지)
        esp_task_wdt_config_t wdt_cfg = {
            .timeout_ms     = 10000,
            .idle_core_mask = 0,
            .trigger_panic  = true,
        };
        esp_task_wdt_reconfigure(&wdt_cfg);
 
        // NVS 초기화 (config_load보다 먼저)
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);
 
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());   // ← wifi_mqtt_init에서 이동
 
        // ── 설정 로드 ───────────────────────────────
        app_config_t cfg = {0};
        config_load(&cfg);
 
        // ── 정상 부팅 루트 ──────────────────────────
        ESP_ERROR_CHECK(uart_modbus_init());
        sensor_data_t sensor = {0};
 
        ESP_ERROR_CHECK(wifi_mqtt_init_with_config(&cfg));   // ← cfg 전달
        ESP_ERROR_CHECK(enc28j60_netif_init(NULL));
        wait_for_ip();
        if (config_portal_check_flag()) {
            config_portal_start();  // 유선 IP로 웹서버 시작
            return;                 // post_handler에서 재시작
        }
 
        ESP_LOGI(TAG, "=== 루프 시작 ===");
        while (1) {
            if (modbus_read_sensor(&sensor) == ESP_OK) {
                mqtt_publish_sensor(sensor.temperature, sensor.humidity);
            }
            vTaskDelay(pdMS_TO_TICKS(cfg.poll_ms));   // ← NVS 값 사용
        }
    }