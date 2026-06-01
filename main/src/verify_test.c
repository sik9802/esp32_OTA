#include "verify_test.h"

static const char *TAG = "VERIFY";

void fc06_verify(void)
{
    uint8_t rx_buf[16];
    uint16_t rx_len;
    sensor_data_t sensor;

    // ① 현재 슬레이브 주소 읽기 (FC03, reg 0x0101)
    ESP_LOGI(TAG, "=== STEP1: Read current slave addr ===");
    modbus_send_fc03(0x01, 0x0101, 1);
    modbus_recv_fc04(rx_buf, &rx_len);   // recv 함수는 FC03/FC04 공용

    // ② FC06으로 같은 값(0x0001) 쓰기 → Echo 검증
    ESP_LOGI(TAG, "=== STEP2: Write slave addr (no change) ===");
    modbus_send_fc06(0x01, 0x0101, 0x0001);
    modbus_recv_fc06(rx_buf, &rx_len);

    // ③ 쓰기 후 다시 읽어서 확인
    ESP_LOGI(TAG, "=== STEP3: Verify via FC03 ===");
    modbus_send_fc03(0x01, 0x0101, 1);
    modbus_recv_fc04(rx_buf, &rx_len);

    // ④ 온습도 정상 동작 확인 (설정 변경 안 했으니 그대로여야 함)
    ESP_LOGI(TAG, "=== STEP4: Sensor check ===");
    modbus_read_sensor(&sensor);
    ESP_LOGI(TAG, "Temp: %.1f°C  Humi: %.1f%%", sensor.temperature, sensor.humidity);

     ESP_LOGI(TAG, "=== fc06_verify 완료 ===");
   // vTaskDelete(NULL);
}

//void fc16_verify(void)
/*
void fc16_verify(void)
{
    uint8_t  rx_buf[16];
    uint16_t rx_len;

    // ① 현재값 읽기 (FC03, 2개)
    ESP_LOGI(TAG, "=== STEP1: Read 0x0101~0x0102 ===");
    modbus_send_fc03(0x01, 0x0101, 2);
    modbus_recv_fc04(rx_buf, &rx_len);

    // ② FC16으로 같은 값 쓰기 (변경 없이 검증)
    ESP_LOGI(TAG, "=== STEP2: FC16 Write ===");
    uint16_t values[2] = {0x0001, 0x0003};  // 주소=1, Baud=9600
    modbus_send_fc16(0x01, 0x0101, 2, values);
    modbus_recv_fc16(rx_buf, &rx_len);

    // ③ 재확인
    ESP_LOGI(TAG, "=== STEP3: Verify ===");
    modbus_send_fc03(0x01, 0x0101, 2);
    modbus_recv_fc04(rx_buf, &rx_len);

    // ④ 센서 정상 동작 확인
    ESP_LOGI(TAG, "=== STEP4: Sensor check ===");
    sensor_data_t sensor;
    modbus_read_sensor(&sensor);
    ESP_LOGI(TAG, "Temp: %.1f°C  Humi: %.1f%%",
             sensor.temperature, sensor.humidity);
    ESP_LOGI(TAG, "=== fc16_verify 완료 ===");
}
*/


void verify_test(void)
{
    //fc06_verify();
    //fc16_verify();
}