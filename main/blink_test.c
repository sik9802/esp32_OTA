
#include "includes.h"

static const char *TAG = "MAIN";


void app_main(void)
{
    ESP_LOGI(TAG, "=== 앱 시작 ===");

    // MQTT TLS 재연결이 CPU1의 lwIP 태스크를 수 초간 독점할 수 있음.
    // IDLE 태스크 감시를 비활성화해 WDT 오탐을 제거.
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = 10000,
        .idle_core_mask = 0,       // IDLE 태스크 감시 비활성화
        .trigger_panic  = true, //false,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);


    // UART2 RS-485 초기화
    ESP_ERROR_CHECK(uart_modbus_init());
    sensor_data_t sensor = {0};

    ESP_ERROR_CHECK(esp_netif_init());            // 네트워크 레이어 사용하기 위해 호출하는 API

    // ── IP 이벤트 핸들러 등록 (netif_init 전에) ──
   

    ESP_ERROR_CHECK(wifi_mqtt_init());        // event loop 생성 + IP 핸들러 등록
    ESP_ERROR_CHECK(enc28j60_netif_init(NULL)); // ENC28J60 초기화 → IP 이벤트 발생
    //ESP_ERROR_CHECK(mqtt_start());            // IP 할당 대기 → MQTT 연결
    wait_for_ip();   // ← IP 할당 완료까지 대기 (DHCP 끝난 후 루프 진입)
     
    // 슬레이브 ID 스캔
        // modbus_scan_slaves();
    // 기능 검증
    //verify_test();
  
     ESP_LOGI(TAG, "=== 루프 시작 ===");
    // 초기화 확인 후 대기
    while (1) {
        
       if (modbus_read_sensor(&sensor) == ESP_OK) {
            mqtt_publish_sensor(sensor.temperature, sensor.humidity);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
        
        // 디스에이블한 기능
        /*
        // 슬레이브 1번, 레지스터 0x0000, 1개 읽기
        //modbus_send_fc04(0x01, 0x0001, 0x0001);
        //modbus_read_sensor(&sensor);
        //vTaskDelay(pdMS_TO_TICKS(2000));  // 2초마다 요청
        */
    }
     
    }